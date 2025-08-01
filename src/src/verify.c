/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) The Exim Maintainers 2020 - 2024 */
/* Copyright (c) University of Cambridge 1995 - 2023 */
/* See the file NOTICE for conditions of use and distribution. */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Functions concerned with verifying things. The original code for callout
caching was contributed by Kevin Fleming (but I hacked it around a bit). */


#include "exim.h"
#include "transports/smtp.h"

#define CUTTHROUGH_CMD_TIMEOUT  30	/* timeout for cutthrough-routing calls */
#define CUTTHROUGH_DATA_TIMEOUT 60	/* timeout for cutthrough-routing calls */
static smtp_context ctctx;
uschar ctbuffer[8192];


static uschar cutthrough_response(client_conn_ctx *, char, uschar **, int);



/*************************************************
*          Retrieve a callout cache record       *
*************************************************/

/* If a record exists, check whether it has expired.

Arguments:
  dbm_file          an open hints file
  key               the record key
  type              "address" or "domain"
  positive_expire   expire time for positive records
  negative_expire   expire time for negative records

Returns:            the cache record if a non-expired one exists, else NULL
*/

static dbdata_callout_cache *
get_callout_cache_record(open_db *dbm_file, const uschar *key, uschar *type,
  int positive_expire, int negative_expire)
{
BOOL negative;
int length, expire;
time_t now;
dbdata_callout_cache *cache_record;

if (!(cache_record = dbfn_read_with_length(dbm_file, key, &length)))
  {
  HDEBUG(D_verify) debug_printf_indent("callout cache: no %s record found for %s\n", type, key);
  return NULL;
  }

/* We treat a record as "negative" if its result field is not positive, or if
it is a domain record and the postmaster field is negative. */

negative = cache_record->result != ccache_accept ||
  (type[0] == 'd' && cache_record->postmaster_result == ccache_reject);
expire = negative? negative_expire : positive_expire;
now = time(NULL);

if (now - cache_record->time_stamp > expire)
  {
  HDEBUG(D_verify) debug_printf_indent("callout cache: %s record expired for %s\n", type, key);
  return NULL;
  }

/* If this is a non-reject domain record, check for the obsolete format version
that doesn't have the postmaster and random timestamps, by looking at the
length. If so, copy it to a new-style block, replicating the record's
timestamp. Then check the additional timestamps. (There's no point wasting
effort if connections are rejected.) */

if (type[0] == 'd' && cache_record->result != ccache_reject)
  {
  if (length == sizeof(dbdata_callout_cache_obs))
    {
    dbdata_callout_cache * new = store_get(sizeof(dbdata_callout_cache), GET_UNTAINTED);
    memcpy(new, cache_record, length);
    new->postmaster_stamp = new->random_stamp = new->time_stamp;
    cache_record = new;
    }

  if (now - cache_record->postmaster_stamp > expire)
    cache_record->postmaster_result = ccache_unknown;

  if (now - cache_record->random_stamp > expire)
    cache_record->random_result = ccache_unknown;
  }

HDEBUG(D_verify) debug_printf_indent("callout cache: found %s record for %s\n", type, key);
return cache_record;
}



/* Check the callout cache.
Options * pm_mailfrom may be modified by cache partial results.

Return: TRUE if result found
*/

static BOOL
cached_callout_lookup(address_item * addr, const uschar * address_key,
  const uschar * from_address, int * opt_ptr, uschar ** pm_ptr,
  int * yield, uschar ** failure_ptr,
  dbdata_callout_cache * new_domain_record, int * old_domain_res)
{
int options = *opt_ptr;
open_db dbblock;
open_db *dbm_file = NULL;

/* Open the callout cache database, if it exists, for reading only at this
stage, unless caching has been disabled. */

if (options & vopt_callout_no_cache)
  {
  HDEBUG(D_verify) debug_printf_indent("callout cache: disabled by no_cache\n");
  }
else if (!(dbm_file = dbfn_open(US"callout", O_RDWR|O_CREAT, &dbblock, FALSE, TRUE)))
  {
  HDEBUG(D_verify) debug_printf_indent("callout cache: not available\n");
  }
else
  {
  /* If a cache database is available see if we can avoid the need to do an
  actual callout by making use of previously-obtained data. */

  const dbdata_callout_cache_address * cache_address_record;
  const dbdata_callout_cache * cache_record = get_callout_cache_record(dbm_file,
      addr->domain, US"domain",
      callout_cache_domain_positive_expire, callout_cache_domain_negative_expire);

  /* If an unexpired cache record was found for this domain, see if the callout
  process can be short-circuited. */

  if (cache_record)
    {
    /* In most cases, if an early command (up to and including MAIL FROM:<>)
    was rejected, there is no point carrying on. The callout fails. However, if
    we are doing a recipient verification with use_sender or use_postmaster
    set, a previous failure of MAIL FROM:<> doesn't count, because this time we
    will be using a non-empty sender. We have to remember this situation so as
    not to disturb the cached domain value if this whole verification succeeds
    (we don't want it turning into "accept"). */

    *old_domain_res = cache_record->result;

    if (  cache_record->result == ccache_reject
       || *from_address == 0 && cache_record->result == ccache_reject_mfnull)
      {
      HDEBUG(D_verify)
	debug_printf_indent("callout cache: domain gave initial rejection, or "
	  "does not accept HELO or MAIL FROM:<>\n");
      setflag(addr, af_verify_nsfail);
      addr->user_message = US"(result of an earlier callout reused).";
      *yield = FAIL;
      *failure_ptr = US"mail";
      dbfn_close(dbm_file);
      return TRUE;
      }

    /* If a previous check on a "random" local part was accepted, we assume
    that the server does not do any checking on local parts. There is therefore
    no point in doing the callout, because it will always be successful. If a
    random check previously failed, arrange not to do it again, but preserve
    the data in the new record. If a random check is required but hasn't been
    done, skip the remaining cache processing. */

    if (options & vopt_callout_random) switch(cache_record->random_result)
      {
      case ccache_accept:
	HDEBUG(D_verify)
	  debug_printf_indent("callout cache: domain accepts random addresses\n");
	*failure_ptr = US"random";
	dbfn_close(dbm_file);
	return TRUE;     /* Default yield is OK */

      case ccache_reject:
	HDEBUG(D_verify)
	  debug_printf_indent("callout cache: domain rejects random addresses\n");
	*opt_ptr = options & ~vopt_callout_random;
	new_domain_record->random_result = ccache_reject;
	new_domain_record->random_stamp = cache_record->random_stamp;
	break;

      default:
	HDEBUG(D_verify)
	  debug_printf_indent("callout cache: need to check random address handling "
	    "(not cached or cache expired)\n");
	dbfn_close(dbm_file);
	return FALSE;
      }

    /* If a postmaster check is requested, but there was a previous failure,
    there is again no point in carrying on. If a postmaster check is required,
    but has not been done before, we are going to have to do a callout, so skip
    remaining cache processing. */

    if (*pm_ptr)
      {
      if (cache_record->postmaster_result == ccache_reject)
	{
	setflag(addr, af_verify_pmfail);
	HDEBUG(D_verify)
	  debug_printf_indent("callout cache: domain does not accept "
	    "RCPT TO:<postmaster@domain>\n");
	*yield = FAIL;
	*failure_ptr = US"postmaster";
	setflag(addr, af_verify_pmfail);
	addr->user_message = US"(result of earlier verification reused).";
	dbfn_close(dbm_file);
	return TRUE;
	}
      if (cache_record->postmaster_result == ccache_unknown)
	{
	HDEBUG(D_verify)
	  debug_printf_indent("callout cache: need to check RCPT "
	    "TO:<postmaster@domain> (not cached or cache expired)\n");
	dbfn_close(dbm_file);
	return FALSE;
	}

      /* If cache says OK, set pm_mailfrom NULL to prevent a redundant
      postmaster check if the address itself has to be checked. Also ensure
      that the value in the cache record is preserved (with its old timestamp).
      */

      HDEBUG(D_verify) debug_printf_indent("callout cache: domain accepts RCPT "
	"TO:<postmaster@domain>\n");
      *pm_ptr = NULL;
      new_domain_record->postmaster_result = ccache_accept;
      new_domain_record->postmaster_stamp = cache_record->postmaster_stamp;
      }
    }

  /* We can't give a result based on information about the domain. See if there
  is an unexpired cache record for this specific address (combined with the
  sender address if we are doing a recipient callout with a non-empty sender).
  */

  if (!(cache_address_record = (dbdata_callout_cache_address *)
    get_callout_cache_record(dbm_file, address_key, US"address",
      callout_cache_positive_expire, callout_cache_negative_expire)))
    {
    dbfn_close(dbm_file);
    return FALSE;
    }

  if (cache_address_record->result == ccache_accept)
    {
    HDEBUG(D_verify)
      debug_printf_indent("callout cache: address record is positive\n");
    }
  else
    {
    HDEBUG(D_verify)
      debug_printf_indent("callout cache: address record is negative\n");
    addr->user_message = US"Previous (cached) callout verification failure";
    *failure_ptr = US"recipient";
    *yield = FAIL;
    }

  /* Close the cache database while we actually do the callout for real. */

  dbfn_close(dbm_file);
  return TRUE;
  }
return FALSE;
}


/* Write results to callout cache
*/
static void
cache_callout_write(dbdata_callout_cache * dom_rec, const uschar * domain,
  int done, dbdata_callout_cache_address * addr_rec, const uschar * address_key)
{
open_db dbblock;
open_db * dbm_file = NULL;

/* If we get here with done == TRUE, a successful callout happened, and yield
will be set OK or FAIL according to the response to the RCPT command.
Otherwise, we looped through the hosts but couldn't complete the business.
However, there may be domain-specific information to cache in both cases.

The value of the result field in the new_domain record is ccache_unknown if
there was an error before or with MAIL FROM:, and errno was not zero,
implying some kind of I/O error. We don't want to write the cache in that case.
Otherwise the value is ccache_accept, ccache_reject, or ccache_reject_mfnull. */

if (dom_rec->result != ccache_unknown)
  if (!(dbm_file = dbfn_open(US"callout", O_RDWR|O_CREAT, &dbblock, FALSE, TRUE)))
    {
    HDEBUG(D_verify) debug_printf_indent("callout cache: not available\n");
    }
  else
    {
    (void)dbfn_write(dbm_file, domain, dom_rec,
      (int)sizeof(dbdata_callout_cache));
    HDEBUG(D_verify) debug_printf_indent("wrote callout cache domain record for %s:\n"
      "  result=%d postmaster=%d random=%d\n",
      domain,
      dom_rec->result,
      dom_rec->postmaster_result,
      dom_rec->random_result);
    }

/* If a definite result was obtained for the callout, cache it unless caching
is disabled. */

if (done  &&  addr_rec->result != ccache_unknown)
  {
  if (!dbm_file)
    dbm_file = dbfn_open(US"callout", O_RDWR|O_CREAT, &dbblock, FALSE, TRUE);
  if (!dbm_file)
    {
    HDEBUG(D_verify) debug_printf_indent("no callout cache available\n");
    }
  else
    {
    (void)dbfn_write(dbm_file, address_key, addr_rec,
      (int)sizeof(dbdata_callout_cache_address));
    HDEBUG(D_verify) debug_printf_indent("wrote %s callout cache address record for %s\n",
      addr_rec->result == ccache_accept ? "positive" : "negative",
      address_key);
    }
  }

if (dbm_file) dbfn_close(dbm_file);
}


/* Cutthrough-multi.  If the existing cached cutthrough connection matches
the one we would make for a subsequent recipient, use it.  Send the RCPT TO
and check the result, nonpipelined as it may be wanted immediately for
recipient-verification.

It seems simpler to deal with this case separately from the main callout loop.
We will need to remember it has sent, or not, so that rcpt-acl tail code
can do it there for the non-rcpt-verify case.  For this we keep an addresscount.

Return: TRUE for a definitive result for the recipient
*/
static int
cutthrough_multi(address_item * addr, host_item * host_list,
  transport_feedback * tf, int * yield)
{
BOOL done = FALSE;

if (addr->transport == cutthrough.addr.transport)
  for (host_item * host = host_list; host; host = host->next)
    if (Ustrcmp(host->address, cutthrough.host.address) == 0)
      {
      int port = 25, host_af;
      const uschar * interface = NULL;  /* Outgoing i/f to use; NULL => any */

      deliver_host = host->name;
      deliver_host_address = host->address;
      deliver_host_port = host->port;
      deliver_domain = addr->domain;
      transport_name = addr->transport->drinst.name;

      host_af = Ustrchr(host->address, ':') ? AF_INET6 : AF_INET;

      GET_OPTION("interface");
      if (  !smtp_get_interface(tf->interface, host_af, addr, &interface,
	      US"callout")
	 || (port = smtp_get_port(tf->port, addr, US"callout")) < 0
	 )
	log_write(0, LOG_MAIN|LOG_PANIC, "<%s>: %s", addr->address,
	  addr->message);

      smtp_port_for_connect(host, port);

      if (  (  interface == cutthrough.interface
	    || (  interface
	       && cutthrough.interface
	       && Ustrcmp(interface, cutthrough.interface) == 0
	    )  )
	 && host->port == cutthrough.host.port
	 )
	{
	uschar * resp = NULL;

	/* Match!  Send the RCPT TO, set done from the response */

	DEBUG(D_verify)
	  debug_printf("already-open verify connection matches recipient\n");

	done =
	     smtp_write_command(&ctctx, SCMD_FLUSH, "RCPT TO:<%.1000s>\r\n",
	      transport_rcpt_address(addr,
		 addr->transport->rcpt_include_affixes)) >= 0
	  && cutthrough_response(&cutthrough.cctx, '2', &resp,
	      CUTTHROUGH_DATA_TIMEOUT) == '2';

	/* This would go horribly wrong if a callout fail was ignored by ACL.
	We punt by abandoning cutthrough on a reject, like the
	first-rcpt does. */

	if (done)
	  {
	  address_item * na = store_get(sizeof(address_item), GET_UNTAINTED);
	  *na = cutthrough.addr;
	  cutthrough.addr = *addr;
	  cutthrough.addr.host_used = &cutthrough.host;
	  cutthrough.addr.next = na;

	  cutthrough.nrcpt++;
	  }
	else
	  {
	  cancel_cutthrough_connection(TRUE, US"recipient rejected");
	  if (!resp || errno == ETIMEDOUT)
	    {
	    HDEBUG(D_verify) debug_printf("SMTP timeout\n");
	    }
	  else if (errno == 0)
	    {
	    if (*resp == 0)
	      Ustrcpy(resp, US"connection dropped");

	    addr->message =
	      string_sprintf("response to %q was: %s",
		big_buffer, string_printing(resp));

	    addr->user_message =
	      string_sprintf("Callout verification failed:\n%s", resp);

	    /* Hard rejection ends the process */

	    if (resp[0] == '5')   /* Address rejected */
	      {
	      *yield = FAIL;
	      done = TRUE;
	      }
	    }
	  }
	}
      break;	/* host_list */
      }
return done;
}




/* A rcpt callout, or cached record of one, verified the address.
Set $domain_data and $local_part_data to detainted versions.
*/
static void
callout_verified_rcpt(const address_item * addr)
{
address_item a = {.address = addr->address};
if (deliver_split_address(&a) != OK) return;
deliver_localpart_data = string_copy_taint(a.local_part, GET_UNTAINTED);
deliver_domain_data =    string_copy_taint(a.domain,     GET_UNTAINTED);
}


/*************************************************
*      Do callout verification for an address    *
*************************************************/

/* This function is called from verify_address() when the address has routed to
a host list, and a callout has been requested. Callouts are expensive; that is
why a cache is used to improve the efficiency.

Arguments:
  addr              the address that's been routed
  host_list         the list of hosts to try
  tf                the transport feedback block

  ifstring          "interface" option from transport, or NULL
  portstring        "port" option from transport, or NULL
  protocolstring    "protocol" option from transport, or NULL
  callout           the per-command callout timeout
  callout_overall   the overall callout timeout (if < 0 use 4*callout)
  callout_connect   the callout connection timeout (if < 0 use callout)
  options           the verification options - these bits are used:
                      vopt_is_recipient => this is a recipient address
                      vopt_callout_no_cache => don't use callout cache
                      vopt_callout_fullpm => if postmaster check, do full one
                      vopt_callout_random => do the "random" thing
                      vopt_callout_recipsender => use original sender addres
                      vopt_callout_r_pmaster   => use postmaster as sender
		      vopt_callout_r_tptsender => use sender defined by tpt
		      vopt_callout_hold        => lazy close connection
  se_mailfrom         MAIL FROM address for sender verify; NULL => ""
  pm_mailfrom         if non-NULL, do the postmaster check with this sender

Returns:            OK/FAIL/DEFER
*/

static int
do_callout(address_item *addr, host_item *host_list, transport_feedback *tf,
  int callout, int callout_overall, int callout_connect, int options,
  uschar *se_mailfrom, uschar *pm_mailfrom)
{
int yield = OK;
int old_domain_cache_result = ccache_accept;
BOOL done = FALSE;
const uschar * address_key;
const uschar * from_address;
uschar * random_local_part = NULL;
const uschar * save_deliver_domain = deliver_domain;
uschar ** failure_ptr = options & vopt_is_recipient
  ? &recipient_verify_failure : &sender_verify_failure;
dbdata_callout_cache new_domain_record;
dbdata_callout_cache_address new_address_record;
time_t callout_start_time;

new_domain_record.result = ccache_unknown;
new_domain_record.postmaster_result = ccache_unknown;
new_domain_record.random_result = ccache_unknown;

memset(&new_address_record, 0, sizeof(new_address_record));

/* For a recipient callout, the key used for the address cache record must
include the sender address if we are using anything but a blank sender in the
callout, because that may influence the result of the callout. */

if (options & vopt_is_recipient)
  if (options & ( vopt_callout_recipsender
		| vopt_callout_r_tptsender
		| vopt_callout_r_pmaster)
     )
    {
    if (options & vopt_callout_recipsender)
      from_address = sender_address;
    else if (options & vopt_callout_r_tptsender)
      {
      transport_instance * tp = addr->transport;
      from_address = addr->prop.errors_address
		  ? addr->prop.errors_address : sender_address;
      DEBUG(D_verify)
	debug_printf(" return-path from routed addr: %s\n", from_address);

      GET_OPTION("return_path");
      if (tp->return_path)
	{
	uschar * new_return_path = expand_string(tp->return_path);
	if (new_return_path)
	  from_address = new_return_path;
	else if (!f.expand_string_forcedfail)
	  return DEFER;
	DEBUG(D_verify)
	  debug_printf(" return-path from transport: %s\n", from_address);
	}
      }
    else /* if (options & vopt_callout_recippmaster) */
      from_address = string_sprintf("postmaster@%s", qualify_domain_sender);

    address_key = string_sprintf("%s/<%s>", addr->address, from_address);
    addr->return_path = from_address;		/* for cutthrough logging */
    if (cutthrough.delivery)			/* cutthrough previously req. */
      options |= vopt_callout_no_cache;		/* in case called by verify= */
    }
  else
    {
    from_address = US"";
    address_key = addr->address;
    }

/* For a sender callout, we must adjust the key if the mailfrom address is not
empty. */

else
  {
  from_address = se_mailfrom ? se_mailfrom : US"";
  address_key = *from_address
    ? string_sprintf("%s/<%s>", addr->address, from_address) : addr->address;
  }

if (cached_callout_lookup(addr, address_key, from_address,
      &options, &pm_mailfrom, &yield, failure_ptr,
      &new_domain_record, &old_domain_cache_result))
  {
  cancel_cutthrough_connection(TRUE, US"cache-hit");
  goto END_CALLOUT;
  }

if (!addr->transport)
  {
  HDEBUG(D_verify) debug_printf("cannot callout via null transport\n");
  }

else if (Ustrcmp(addr->transport->drinst.driver_name, "smtp") != 0)
  log_write(0, LOG_MAIN|LOG_PANIC|LOG_CONFIG_FOR, "callout transport '%s': %s is non-smtp",
    addr->transport->drinst.name, addr->transport->drinst.driver_name);
else
  {
  smtp_transport_options_block * ob = addr->transport->drinst.options_block;
  smtp_context * sx = NULL;

  /* The information wasn't available in the cache, so we have to do a real
  callout and save the result in the cache for next time, unless no_cache is set,
  or unless we have a previously cached negative random result. If we are to test
  with a random local part, ensure that such a local part is available. If not,
  log the fact, but carry on without randomising. */

  if (options & vopt_callout_random)
    {
    GET_OPTION("callout_random_local_part");
    if (  callout_random_local_part
       && !(random_local_part = expand_string(callout_random_local_part)))
      log_write(0, LOG_MAIN|LOG_PANIC, "failed to expand "
        "callout_random_local_part: %s", expand_string_message);
    }

  /* Compile regex' used by client-side smtp */

  smtp_deliver_init();

  /* Default the connect and overall callout timeouts if not set, and record the
  time we are starting so that we can enforce it. */

  if (callout_overall < 0) callout_overall = 4 * callout;
  if (callout_connect < 0) callout_connect = callout;
  callout_start_time = time(NULL);

  /* Before doing a real callout, if this is an SMTP connection, flush the SMTP
  output because a callout might take some time. When PIPELINING is active and
  there are many recipients, the total time for doing lots of callouts can add
  up and cause the client to time out. So in this case we forgo the PIPELINING
  optimization. */

  if (smtp_out_fd >= 0 && !f.disable_callout_flush) smtp_fflush(SFF_UNCORK);

  clearflag(addr, af_verify_pmfail);  /* postmaster callout flag */
  clearflag(addr, af_verify_nsfail);  /* null sender callout flag */

/* cutthrough-multi: if a nonfirst rcpt has the same routing as the first,
and we are holding a cutthrough conn open, we can just append the rcpt to
that conn for verification purposes (and later delivery also).  Simplest
coding means skipping this whole loop and doing the append separately.  */

  /* Can we re-use an open cutthrough connection? */

  if (cutthrough.cctx.sock >= 0)
    {
    if (  !(options & vopt_callout_r_pmaster)
       && !random_local_part
       && !pm_mailfrom
       && Ustrcmp(addr->return_path, cutthrough.addr.return_path) == 0
       )
      done = cutthrough_multi(addr, host_list, tf, &yield);

    if (!done)
      cancel_cutthrough_connection(TRUE, US"incompatible connection");
    }

  /* If we did not use a cached connection, make connections to the hosts
  and do real callouts. The list of hosts is passed in as an argument. */

  for (host_item * host = host_list; host && !done; host = host->next)
    {
    int port = 25, host_af;
    const uschar * interface = NULL;  /* Outgoing interface to use; NULL=>any */

    if (!host->address)
      {
      DEBUG(D_verify) debug_printf("no IP address for host name %s: skipping\n",
        host->name);
      continue;
      }

    /* Check the overall callout timeout */

    if (time(NULL) - callout_start_time >= callout_overall)
      {
      HDEBUG(D_verify) debug_printf("overall timeout for callout exceeded\n");
      break;
      }

    /* Set IPv4 or IPv6 */

    host_af = Ustrchr(host->address, ':') ? AF_INET6 : AF_INET;

    /* Expand and interpret the interface and port strings. The latter will not
    be used if there is a host-specific port (e.g. from a manualroute router).
    This has to be delayed till now, because they may expand differently for
    different hosts. If there's a failure, log it, but carry on with the
    defaults. */

    deliver_host = host->name;
    deliver_host_address = host->address;
    deliver_host_port = host->port;
    deliver_domain = addr->domain;
    transport_name = addr->transport->drinst.name;

    GET_OPTION("interface");
    GET_OPTION("port");
    if (  !smtp_get_interface(tf->interface, host_af, addr, &interface,
            US"callout")
       || (port = smtp_get_port(tf->port, addr, US"callout")) < 0
       )
      log_write(0, LOG_MAIN|LOG_PANIC, "<%s>: %s", addr->address,
        addr->message);

    if (!sx) sx = store_get(sizeof(*sx), GET_TAINTED);	/* tainted buffers */
    memset(sx, 0, sizeof(*sx));

    sx->addrlist = sx->first_addr = addr;
    sx->conn_args.host = host;
    sx->conn_args.host_af = host_af,
    sx->port = port;
    sx->conn_args.interface = interface;
    sx->helo_data = tf->helo_data;
    sx->conn_args.tblock = addr->transport;
    sx->cctx.sock = sx->conn_args.sock = -1;
    sx->verify = TRUE;

tls_retry_connection:
    /* Set the address state so that errors are recorded in it */

    addr->transport_return = PENDING_DEFER;
    ob->connect_timeout = callout_connect;
    ob->command_timeout = callout;

    /* Get the channel set up ready for a message (MAIL FROM being the next
    SMTP command to send.  If we tried TLS but it failed, try again without
    if permitted */

    yield = smtp_setup_conn(sx, FALSE);
#ifndef DISABLE_TLS
    if (  yield == DEFER
       && addr->basic_errno == ERRNO_TLSFAILURE
       && ob->tls_tempfail_tryclear
       && verify_check_given_host(CUSS &ob->hosts_require_tls, host) != OK
       )
      {
      log_write(0, LOG_MAIN,
	"%s: callout unencrypted to %s [%s] (not in hosts_require_tls)",
	addr->message, host->name, host->address);
      addr->transport_return = PENDING_DEFER;
      yield = smtp_setup_conn(sx, TRUE);
      }
#endif
    if (yield != OK)
      {
      smtp_debug_cmd_report();	/*XXX we seem to exit without what should
				be a common call to this.  How? */
      errno = addr->basic_errno;

      /* For certain errors we want specifically to log the transport name,
      for ease of fixing config errors. Slightly ugly doing it here, but we want
      to not leak that also in the SMTP response. */
      switch (errno)
	{
	case EPROTOTYPE:
	case ENOPROTOOPT:
	case EPROTONOSUPPORT:
	case ESOCKTNOSUPPORT:
	case EOPNOTSUPP:
	case EPFNOSUPPORT:
	case EAFNOSUPPORT:
	case EADDRINUSE:
	case EADDRNOTAVAIL:
	case ENETDOWN:
	case ENETUNREACH:
	case EINVAL:			/* OpenBSD gives this for netunreach */
	  log_write(0, LOG_MAIN|LOG_PANIC,
	    "%s verify %s (making callout connection): T=%s %s",
	    options & vopt_is_recipient ? "sender" : "recipient",
	    yield == FAIL ? "fail" : "defer",
	    transport_name, strerror(errno));
	}

      transport_name = NULL;
      deliver_host = deliver_host_address = NULL;
      deliver_domain = save_deliver_domain;

      /* Failure to accept HELO is cached; this blocks the whole domain for all
      senders. I/O errors and defer responses are not cached. */

      if (yield == FAIL && (errno == 0 || errno == ERRNO_SMTPCLOSED))
	{
	setflag(addr, af_verify_nsfail);
	new_domain_record.result = ccache_reject;
	done = TRUE;
	}
      else
	done = FALSE;
      goto no_conn;
      }

    /* If we needed to authenticate, smtp_setup_conn() did that.  Copy
    the AUTH info for logging */

    addr->authenticator = client_authenticator;
    addr->auth_id = client_authenticated_id;

    sx->from_addr = from_address;
    sx->first_addr = sx->sync_addr = addr;
    sx->ok = FALSE;			/*XXX these 3 last might not be needed for verify? */
    sx->send_rset = TRUE;
    sx->completed_addr = FALSE;

/*XXX do not want to write a cache record for ATRN */
    new_domain_record.result = old_domain_cache_result == ccache_reject_mfnull
      ? ccache_reject_mfnull : ccache_accept;

    /* Do the random local part check first. Temporarily replace the recipient
    with the "random" value */

    if (random_local_part)
      {
      const uschar * main_address = addr->address;
      const uschar * rcpt_domain = addr->domain;

#ifdef SUPPORT_I18N
      uschar * errstr = NULL;
      if (  testflag(addr, af_utf8_downcvt)
	 && (rcpt_domain = string_domain_utf8_to_alabel(rcpt_domain,
				    &errstr), errstr)
	 )
	{
	addr->message = errstr;
	errno = ERRNO_EXPANDFAIL;
	setflag(addr, af_verify_nsfail);
	done = FALSE;
	rcpt_domain = US"";  /*XXX errorhandling! */
	}
#endif

      /* This would be ok for 1st rcpt of a cutthrough (the case handled here;
      subsequents are done in cutthrough_multi()), but no way to
      handle a subsequent because of the RSET vaporising the MAIL FROM.
      So refuse to support any.  Most cutthrough use will not involve
      random_local_part, so no loss. */
      cancel_cutthrough_connection(TRUE, US"random-recipient");

      addr->address = string_sprintf("%s@%.1000s",
				    random_local_part, rcpt_domain);
      done = FALSE;

      /* If accepted, we aren't going to do any further tests below.
      Otherwise, cache a real negative response, and get back to the right
      state to send RCPT. Unless there's some problem such as a dropped
      connection, we expect to succeed, because the commands succeeded above.
      However, some servers drop the connection after responding to an
      invalid recipient, so on (any) error we drop and remake the connection.
      XXX We don't care about that for postmaster_full.  Should we?

      XXX could we add another flag to the context, and have the common
      code emit the RSET too?  Even pipelined after the RCPT...
      Then the main-verify call could use it if there's to be a subsequent
      postmaster-verify.
      The sync_responses() would need to be taught about it and we'd
      need another return code filtering out to here.

      Avoid using a SIZE option on the MAIL for all random-rcpt checks.
      */

      sx->avoid_option = OPTION_SIZE;

      /* Remember when we last did a random test */
      new_domain_record.random_stamp = time(NULL);

      if (smtp_write_mail_and_rcpt_cmds(sx, &yield) == sw_mrc_ok)
	switch(addr->transport_return)
	  {
	  case PENDING_OK:	/* random was accepted, unfortunately */
	    new_domain_record.random_result = ccache_accept;
	    yield = OK;		/* Only usable verify result we can return */
	    done = TRUE;
	    *failure_ptr = US"random";
	    goto no_conn;
	  case FAIL:		/* rejected: the preferred result */
	    new_domain_record.random_result = ccache_reject;
	    sx->avoid_option = 0;

	    /* Between each check, issue RSET, because some servers accept only
	    one recipient after MAIL FROM:<>.
	    XXX We don't care about that for postmaster_full.  Should we? */

	    if ((done =
	      smtp_write_command(sx, SCMD_FLUSH, "RSET\r\n") >= 0
	      &&
	      smtp_read_response(sx, sx->buffer, sizeof(sx->buffer),
				'2', callout)))
	      break;

	    HDEBUG(D_acl|D_v)
	      debug_printf_indent("problem after random/rset/mfrom; reopen conn\n");
	    random_local_part = NULL;
#ifndef DISABLE_TLS
	    tls_close(sx->cctx.tls_ctx, TLS_SHUTDOWN_NOWAIT);
#endif
	    HDEBUG(D_transport|D_acl|D_v) debug_printf_indent("  SMTP(close)>>\n");
	    (void)close(sx->cctx.sock);
	    sx->cctx.sock = -1;
#ifndef DISABLE_EVENT
	    (void) event_raise(addr->transport->event_action,
			      US"tcp:close", NULL, NULL);
#endif
	    addr->address = main_address;
	    addr->transport_return = PENDING_DEFER;
	    sx->first_addr = sx->sync_addr = addr;
	    sx->ok = FALSE;
	    sx->send_rset = TRUE;
	    sx->completed_addr = FALSE;
	    goto tls_retry_connection;
	  case DEFER:		/* 4xx response to random */
	    break;		/* Just to be clear. ccache_unknown, !done. */
	  }

      /* Re-setup for main verify, or for the error message when failing */
      addr->address = main_address;
      addr->transport_return = PENDING_DEFER;
      sx->first_addr = sx->sync_addr = addr;
      sx->ok = FALSE;
      sx->send_rset = TRUE;
      sx->completed_addr = FALSE;
      }
    else
      done = TRUE;

    /* Main verify.  For rcpt-verify use SIZE if we know it and we're not cacheing;
    for sndr-verify never use it. */

    if (done && !(options & vopt_atrn))
      {
      if (!(options & vopt_is_recipient  &&  options & vopt_callout_no_cache))
	sx->avoid_option = OPTION_SIZE;

      done = FALSE;
      switch(smtp_write_mail_and_rcpt_cmds(sx, &yield))
	{
	case sw_mrc_ok:
	  switch(addr->transport_return)	/* ok so far */
	    {
	    case PENDING_OK:  done = TRUE;
			      new_address_record.result = ccache_accept;
			      break;
	    case FAIL:	      done = TRUE;
			      yield = FAIL;
			      *failure_ptr = US"recipient";
			      new_address_record.result = ccache_reject;
			      break;
	    default:	      break;
	    }
	  break;

	case sw_mrc_bad_mail:			/* MAIL response error */
	  *failure_ptr = US"mail";
	  if (errno == 0 && sx->buffer[0] == '5')
	    {
	    setflag(addr, af_verify_nsfail);
	    if (from_address[0] == 0)
	      new_domain_record.result = ccache_reject_mfnull;
	    }
	  break;
					/* non-MAIL read i/o error */
					/* non-MAIL response timeout */
					/* internal error; channel still usable */
	default:  break;		/* transmit failed */
	}
      }

    addr->auth_sndr = client_authenticated_sender;

    deliver_host = deliver_host_address = NULL;
    deliver_domain = save_deliver_domain;

    /* Do postmaster check if requested; if a full check is required, we
    check for RCPT TO:<postmaster> (no domain) in accordance with RFC 821. */

    if (done && pm_mailfrom)
      {
      /* Could possibly shift before main verify, just above, and be ok
      for cutthrough.  But no way to handle a subsequent rcpt, so just
      refuse any */
      cancel_cutthrough_connection(TRUE, US"postmaster verify");
      HDEBUG(D_acl|D_v) debug_printf_indent("Cutthrough cancelled by presence of postmaster verify\n");

      done = smtp_write_command(sx, SCMD_FLUSH, "RSET\r\n") >= 0
          && smtp_read_response(sx, sx->buffer, sizeof(sx->buffer), '2', callout);

      if (done)
	{
	const uschar * main_address = addr->address;

	/*XXX oops, affixes */
	addr->address = string_sprintf("postmaster@%.1000s", addr->domain);
	addr->transport_return = PENDING_DEFER;

	sx->from_addr = pm_mailfrom;
	sx->first_addr = sx->sync_addr = addr;
	sx->ok = FALSE;
	sx->send_rset = TRUE;
	sx->completed_addr = FALSE;
	sx->avoid_option = OPTION_SIZE;

	if(  smtp_write_mail_and_rcpt_cmds(sx, &yield) == sw_mrc_ok
	  && addr->transport_return == PENDING_OK
	  )
	  done = TRUE;
	else
	  done = (options & vopt_callout_fullpm) != 0
	      && smtp_write_command(sx, SCMD_FLUSH,
			    "RCPT TO:<postmaster>\r\n") >= 0
	      && smtp_read_response(sx, sx->buffer,
			    sizeof(sx->buffer), '2', callout);

	/* Sort out the cache record */

	new_domain_record.postmaster_stamp = time(NULL);

	if (done)
	  new_domain_record.postmaster_result = ccache_accept;
	else if (errno == 0 && sx->buffer[0] == '5')
	  {
	  *failure_ptr = US"postmaster";
	  setflag(addr, af_verify_pmfail);
	  new_domain_record.postmaster_result = ccache_reject;
	  }

	addr->address = main_address;
	}
      }
    /* For any failure of the main check, other than a negative response, we just
    close the connection and carry on. We can identify a negative response by the
    fact that errno is zero. For I/O errors it will be non-zero

    Set up different error texts for logging and for sending back to the caller
    as an SMTP response. Log in all cases, using a one-line format. For sender
    callouts, give a full response to the caller, but for recipient callouts,
    don't give the IP address because this may be an internal host whose identity
    is not to be widely broadcast. */

no_conn:
    switch(errno)
      {
      case ETIMEDOUT:
	HDEBUG(D_verify) debug_printf("SMTP timeout\n");
	sx->send_quit = FALSE;
	break;

#ifdef SUPPORT_I18N
      case ERRNO_UTF8_FWD:
	{
	errno = 0;
	addr->message = US"response to \"EHLO\" did not include SMTPUTF8";
	addr->user_message = acl_where == ACL_WHERE_RCPT
	  ? US"533 no support for internationalised mailbox name"
	  : US"550 mailbox unavailable";
	yield = FAIL;
	done = TRUE;
	}
	break;
#endif
      case ECONNREFUSED:
	sx->send_quit = FALSE;
	break;

      case 0:
	if (*sx->buffer == 0) Ustrcpy(sx->buffer, US"connection dropped");

	/*XXX test here is ugly; seem to have a split of responsibility for
	building this message.  Need to rationalise.  Where is it done
	before here, and when not?
	Not == 5xx resp to MAIL on main-verify
	*/
	if (!addr->message) addr->message =
	  string_sprintf("response to %q was: %s",
			  big_buffer, string_printing(sx->buffer));

	/* RFC 5321 section 4.2: the text portion of the response may have only
	HT, SP, Printable US-ASCII.  Deal with awkward chars by cutting the
	received message off before passing it onward.  Newlines are ok; they
	just become a multiline response (but wrapped in the error code we
	produce). */

	for (uschar * s = sx->buffer;
	     *s && s < sx->buffer + sizeof(sx->buffer);
	     s++)
	  {
	  uschar c = *s;
	  if (c != '\t' && c != '\n' && (c < ' ' || c > '~'))
	    {
	    if (s - sx->buffer < sizeof(sx->buffer) - 12)
	      memcpy(s, "(truncated)", 12);
	    else
	      *s = '\0';
	    break;
	    }
	  }
	addr->user_message = options & vopt_is_recipient
	  ? string_sprintf("Callout verification failed:\n%s", sx->buffer)
	  : string_sprintf("Called:   %s\nSent:     %s\nResponse: %s",
	    host->address, big_buffer, sx->buffer);

	/* Hard rejection ends the process */

	if (sx->buffer[0] == '5')   /* Address rejected */
	  {
	  yield = FAIL;
	  done = TRUE;
	  }
	break;
      }

    /* End the SMTP conversation and close the connection. */

    /* Cutthrough - on a successful connect and recipient-verify with
    use-sender and we are 1st rcpt and have no cutthrough conn so far
    here is where we want to leave the conn open.  Ditto for a lazy-close
    verify. */

    if (cutthrough.delivery)
      {
      if (expand_string_nonempty(addr->transport->filter_command))
        {
        cutthrough.delivery= FALSE;
        HDEBUG(D_acl|D_v) debug_printf("Cutthrough cancelled by presence of transport filter\n");
        }
#ifndef DISABLE_DKIM
      /* DKIM signing needs to add a header after seeing the whole body, so we
      cannot just copy body bytes to the outbound as they are received, which is
      the intent of cutthrough. */
      if (expand_string_nonempty(ob->dkim.dkim_domain))
        {
        cutthrough.delivery= FALSE;
        HDEBUG(D_acl|D_v) debug_printf("Cutthrough cancelled by presence of DKIM signing\n");
        }
#endif
#ifdef EXPERIMENTAL_ARC
      /* ARC has the same issue as DKIM above */
      if (expand_string_nonempty(ob->arc_sign))
        {
        cutthrough.delivery= FALSE;
        HDEBUG(D_acl|D_v) debug_printf("Cutthrough cancelled by presence of ARC signing\n");
        }
#endif
      }

    if (  (cutthrough.delivery || options & vopt_callout_hold)
       && rcpt_count == 1
       && done
       && yield == OK
       && !(options & (vopt_callout_r_pmaster| vopt_success_on_redirect))
       && !random_local_part
       && !pm_mailfrom
       && cutthrough.cctx.sock < 0
       && !sx->lmtp
       )
      {
      HDEBUG(D_acl|D_v) debug_printf_indent("holding verify callout open for %s\n",
	cutthrough.delivery
	? "cutthrough delivery" : "potential further verifies and delivery");

      cutthrough.callout_hold_only = !cutthrough.delivery;
      cutthrough.is_tls =	tls_out.active.sock >= 0;
      /* We assume no buffer in use in the outblock */
      cutthrough.cctx =		sx->cctx;
      cutthrough.nrcpt =	1;
      cutthrough.transport =	addr->transport->drinst.name;
      cutthrough.interface =	interface;
      cutthrough.snd_port =	sending_port;
      cutthrough.peer_options =	smtp_peer_options;
      cutthrough.host =		*host;
	{
	int oldpool = store_pool;
	store_pool = POOL_PERM;
	cutthrough.snd_ip = string_copy(sending_ip_address);
	cutthrough.host.name = string_copy(host->name);
	cutthrough.host.address = string_copy(host->address);
	store_pool = oldpool;
	}

      /* Save the address_item and parent chain for later logging */
      cutthrough.addr =		*addr;
      cutthrough.addr.next =	NULL;
      cutthrough.addr.host_used = &cutthrough.host;
      for (address_item * caddr = &cutthrough.addr, * parent = addr->parent;
	   parent;
	   caddr = caddr->parent, parent = parent->parent)
        *(caddr->parent = store_get(sizeof(address_item), GET_UNTAINTED)) = *parent;

      ctctx.outblock.buffer = ctbuffer;
      ctctx.outblock.buffersize = sizeof(ctbuffer);
      ctctx.outblock.ptr = ctbuffer;
      /* ctctx.outblock.cmd_count = 0; ctctx.outblock.authenticating = FALSE; */
      ctctx.outblock.cctx = &cutthrough.cctx;
      }
    else
      {
      /* Ensure no cutthrough on multiple verifies that were incompatible */
      if (options & (vopt_callout_recipsender | vopt_callout_r_tptsender))
        cancel_cutthrough_connection(TRUE, US"not usable for cutthrough");
      if (sx->send_quit && sx->cctx.sock >= 0)
	if (smtp_write_command(sx, SCMD_FLUSH, "QUIT\r\n") != -1)
	  /* Wait a short time for response, and discard it */
	  smtp_read_response(sx, sx->buffer, sizeof(sx->buffer), '2', 1);

      if (sx->cctx.sock >= 0)
	{
#ifndef DISABLE_TLS
	if (sx->cctx.tls_ctx)
	  {
	  tls_close(sx->cctx.tls_ctx, TLS_SHUTDOWN_NOWAIT);
	  sx->cctx.tls_ctx = NULL;
	  }
#endif
	HDEBUG(D_transport|D_acl|D_v) debug_printf_indent("  SMTP(close)>>\n");
	(void)close(sx->cctx.sock);
	sx->cctx.sock = -1;
	smtp_debug_cmd_report();
#ifndef DISABLE_EVENT
	(void) event_raise(addr->transport->event_action, US"tcp:close", NULL, NULL);
#endif
	}
      }

    if (!done || yield != OK)
      addr->message = string_sprintf("%s [%s] : %s", host->name, host->address,
				    addr->message);
    }    /* Loop through all hosts, while !done */
  }

/* If we get here with done == TRUE, a successful callout happened, and yield
will be set OK or FAIL according to the response to the RCPT command.
Otherwise, we looped through the hosts but couldn't complete the business.
However, there may be domain-specific information to cache in both cases. */

if (!(options & vopt_callout_no_cache))
  cache_callout_write(&new_domain_record, addr->domain,
    done, &new_address_record, address_key);

/* Failure to connect to any host, or any response other than 2xx or 5xx is a
temporary error. If there was only one host, and a response was received, leave
it alone if supplying details. Otherwise, give a generic response. */

if (!done)
  {
  uschar * dullmsg = string_sprintf("Could not complete %s verify callout",
    options & vopt_is_recipient ? "recipient" : "sender");
  yield = DEFER;

  addr->message = host_list->next || !addr->message
    ? dullmsg : string_sprintf("%s: %s", dullmsg, addr->message);

  addr->user_message = smtp_return_error_details
    ? string_sprintf("%s for <%s>.\n"
      "The mail server(s) for the domain may be temporarily unreachable, or\n"
      "they may be permanently unreachable from this server. In the latter case,\n%s",
      dullmsg, addr->address,
      options & vopt_is_recipient
	? "the address will never be accepted."
        : "you need to change the address or create an MX record for its domain\n"
	  "if it is supposed to be generally accessible from the Internet.\n"
	  "Talk to your mail administrator for details.")
    : dullmsg;

  /* Force a specific error code */

  addr->basic_errno = ERRNO_CALLOUTDEFER;
  }

/* Come here from within the cache-reading code on fast-track exit. */

END_CALLOUT:
if (!(options & vopt_atrn))
  tls_modify_variables(&tls_in);	/* return variables to inbound values */
return yield;
}



/* Called after recipient-acl to get a cutthrough connection open when
   one was requested and a recipient-verify wasn't subsequently done.
*/
int
open_cutthrough_connection(address_item * addr, BOOL transport_sender)
{
address_item addr2;
int vopt, rc;

/* Use a recipient-verify-callout to set up the cutthrough connection. */
/* We must use a copy of the address for verification, because it might
get rewritten. */

addr2 = *addr;
HDEBUG(D_acl) debug_printf_indent("----------- %s cutthrough setup ------------\n",
  rcpt_count > 1 ? "more" : "start");

vopt = transport_sender
  ? vopt_is_recipient | vopt_callout_r_tptsender | vopt_callout_no_cache
  : vopt_is_recipient | vopt_callout_recipsender | vopt_callout_no_cache;

rc = verify_address(&addr2, -1, vopt, CUTTHROUGH_CMD_TIMEOUT, -1, -1,
	NULL, NULL, NULL);
addr->message = addr2.message;
addr->user_message = addr2.user_message;
HDEBUG(D_acl) debug_printf_indent("----------- end cutthrough setup ------------\n");
return rc;
}



/* Send given number of bytes from the buffer */
static BOOL
cutthrough_send(int n)
{
if(cutthrough.cctx.sock < 0)
  return TRUE;

if(
#ifndef DISABLE_TLS
   cutthrough.is_tls
   ? tls_write(cutthrough.cctx.tls_ctx, ctctx.outblock.buffer, n, FALSE)
   :
#endif
     send(cutthrough.cctx.sock, ctctx.outblock.buffer, n, 0) > 0
  )
{
  transport_count += n;
  ctctx.outblock.ptr= ctctx.outblock.buffer;
  return TRUE;
}

HDEBUG(D_transport|D_acl) debug_printf_indent("cutthrough_send failed: %s\n", strerror(errno));
return FALSE;
}



static BOOL
_cutthrough_puts(const uschar * cp, int n)
{
while(n--)
 {
 if(ctctx.outblock.ptr >= ctctx.outblock.buffer+ctctx.outblock.buffersize)
   if(!cutthrough_send(ctctx.outblock.buffersize))
     return FALSE;

 *ctctx.outblock.ptr++ = *cp++;
 }
return TRUE;
}

/* Buffered output of counted data block.   Return boolean success */
static BOOL
cutthrough_puts(const uschar * cp, int n)
{
if (cutthrough.cctx.sock < 0) return TRUE;
if (_cutthrough_puts(cp, n))  return TRUE;
cancel_cutthrough_connection(TRUE, US"transmit failed");
return FALSE;
}

void
cutthrough_data_puts(uschar * cp, int n)
{
if (cutthrough.delivery) (void) cutthrough_puts(cp, n);
return;
}


static BOOL
_cutthrough_flush_send(void)
{
int n = ctctx.outblock.ptr - ctctx.outblock.buffer;

if(n>0)
  if(!cutthrough_send(n))
    return FALSE;
return TRUE;
}


/* Send out any bufferred output.  Return boolean success. */
BOOL
cutthrough_flush_send(void)
{
if (_cutthrough_flush_send()) return TRUE;
cancel_cutthrough_connection(TRUE, US"transmit failed");
return FALSE;
}


static BOOL
cutthrough_put_nl(void)
{
return cutthrough_puts(US"\r\n", 2);
}


void
cutthrough_data_put_nl(void)
{
cutthrough_data_puts(US"\r\n", 2);
}


/* Get and check response from cutthrough target.
Used for
- nonfirst RCPT
- predata
- data finaldot
- cutthrough conn close
*/
static uschar
cutthrough_response(client_conn_ctx * cctx, char expect, uschar ** copy, int timeout)
{
smtp_context sx = {0};
uschar inbuffer[4096];
uschar responsebuffer[4096];

sx.inblock.buffer = inbuffer;
sx.inblock.buffersize = sizeof(inbuffer);
sx.inblock.ptr = inbuffer;
sx.inblock.ptrend = inbuffer;
sx.inblock.cctx = cctx;
if(!smtp_read_response(&sx, responsebuffer, sizeof(responsebuffer), expect, timeout))
  cancel_cutthrough_connection(TRUE, US"unexpected response to smtp command");

if(copy)
  {
  uschar * cp;
  *copy = cp = string_copy(responsebuffer);
  /* Trim the trailing end of line */
  cp += Ustrlen(responsebuffer);
  if(cp > *copy  &&  cp[-1] == '\n') *--cp = '\0';
  if(cp > *copy  &&  cp[-1] == '\r') *--cp = '\0';
  }

return responsebuffer[0];
}


/* Negotiate dataphase with the cutthrough target, returning success boolean */
BOOL
cutthrough_predata(void)
{
if(cutthrough.cctx.sock < 0 || cutthrough.callout_hold_only)
  return FALSE;

smtp_debug_cmd(US"DATA", 0);
cutthrough_puts(US"DATA\r\n", 6);
cutthrough_flush_send();

/* Assume nothing buffered.  If it was it gets ignored. */
return cutthrough_response(&cutthrough.cctx, '3', NULL, CUTTHROUGH_DATA_TIMEOUT) == '3';
}


/* tctx arg only to match write_chunk() */
static BOOL
cutthrough_write_chunk(transport_ctx * tctx, const uschar * s, int len)
{
const uschar * s2;
while(s && (s2 = Ustrchr(s, '\n')))
 {
 if(!cutthrough_puts(s, s2-s) || !cutthrough_put_nl())
  return FALSE;
 s = s2+1;
 }
return TRUE;
}


/* Buffered send of headers.  Return success boolean. */
/* Expands newlines to wire format (CR,NL).           */
/* Also sends header-terminating blank line.          */
BOOL
cutthrough_headers_send(void)
{
transport_ctx tctx;

if(cutthrough.cctx.sock < 0 || cutthrough.callout_hold_only)
  return FALSE;

/* We share a routine with the mainline transport to handle header add/remove/rewrites,
   but having a separate buffered-output function (for now)
*/
HDEBUG(D_acl) debug_printf_indent("----------- start cutthrough headers send -----------\n");

tctx.u.fd = cutthrough.cctx.sock;
tctx.tblock = cutthrough.addr.transport;
tctx.addr = &cutthrough.addr;
tctx.check_string = US".";
tctx.escape_string = US"..";
/*XXX check under spool_files_wireformat.  Might be irrelevant */
tctx.options = topt_use_crlf;

if (!transport_headers_send(&tctx, &cutthrough_write_chunk))
  return FALSE;

HDEBUG(D_acl) debug_printf_indent("----------- done cutthrough headers send ------------\n");
return TRUE;
}


static void
close_cutthrough_connection(const uschar * why)
{
int fd = cutthrough.cctx.sock;
if(fd >= 0)
  {
  /* We could be sending this after a bunch of data, but that is ok as
     the only way to cancel the transfer in dataphase is to drop the tcp
     conn before the final dot.
  */
  client_conn_ctx tmp_ctx = cutthrough.cctx;
  ctctx.outblock.ptr = ctbuffer;
  smtp_debug_cmd(US"QUIT", 0);
  _cutthrough_puts(US"QUIT\r\n", 6);	/* avoid recursion */
  _cutthrough_flush_send();
  cutthrough.cctx.sock = -1;		/* avoid recursion via read timeout */
  cutthrough.nrcpt = 0;			/* permit re-cutthrough on subsequent message */

  /* Wait a short time for response, and discard it */
  cutthrough_response(&tmp_ctx, '2', NULL, 1);

#ifndef DISABLE_TLS
  if (cutthrough.is_tls)
    {
    tls_close(cutthrough.cctx.tls_ctx, TLS_SHUTDOWN_NOWAIT);
    cutthrough.cctx.tls_ctx = NULL;
    cutthrough.is_tls = FALSE;
    }
#endif
  HDEBUG(D_transport|D_acl|D_v) debug_printf_indent("  SMTP(close)>>\n");
  (void)close(fd);
  smtp_debug_cmd_report();
  HDEBUG(D_acl) debug_printf_indent("----------- cutthrough shutdown (%s) ------------\n", why);
  }
ctctx.outblock.ptr = ctbuffer;
}

void
cancel_cutthrough_connection(BOOL close_noncutthrough_verifies, const uschar * why)
{
if (cutthrough.delivery || close_noncutthrough_verifies)
  close_cutthrough_connection(why);
cutthrough.delivery = cutthrough.callout_hold_only = FALSE;
}


void
release_cutthrough_connection(const uschar * why)
{
if (cutthrough.cctx.sock < 0) return;
HDEBUG(D_acl) debug_printf_indent("release cutthrough conn: %s\n", why);
cutthrough.cctx.sock = -1;
cutthrough.cctx.tls_ctx = NULL;
cutthrough.delivery = cutthrough.callout_hold_only = FALSE;
}




/* Have senders final-dot.  Send one to cutthrough target, and grab the response.
   Log an OK response as a transmission.
   Close the connection.
   Return smtp response-class digit.
*/
uschar *
cutthrough_finaldot(void)
{
uschar res;
HDEBUG(D_transport|D_acl|D_v) debug_printf_indent("  SMTP>> .\n");

/* Assume data finshed with new-line */
if(  !cutthrough_puts(US".", 1)
  || !cutthrough_put_nl()
  || !cutthrough_flush_send()
  )
  return cutthrough.addr.message;

res = cutthrough_response(&cutthrough.cctx, '2', &cutthrough.addr.message,
	CUTTHROUGH_DATA_TIMEOUT);
for (address_item * addr = &cutthrough.addr; addr; addr = addr->next)
  {
  addr->message = cutthrough.addr.message;
  switch(res)
    {
    case '2':
      delivery_log(LOG_MAIN, addr, (int)'>', NULL);
      close_cutthrough_connection(US"delivered");
      break;

    case '4':
      delivery_log(LOG_MAIN, addr, 0,
	US"tmp-reject from cutthrough after DATA:");
      break;

    case '5':
      delivery_log(LOG_MAIN|LOG_REJECT, addr, 0,
	US"rejected after DATA:");
      break;

    default:
      break;
    }
  }
return cutthrough.addr.message;
}



/*************************************************
*           Copy error to toplevel address       *
*************************************************/

/* This function is used when a verify fails or defers, to ensure that the
failure or defer information is in the original toplevel address. This applies
when an address is redirected to a single new address, and the failure or
deferral happens to the child address.

Arguments:
  vaddr       the verify address item
  addr        the final address item
  yield       FAIL or DEFER

Returns:      the value of YIELD
*/

static int
copy_error(address_item *vaddr, address_item *addr, int yield)
{
if (addr != vaddr)
  {
  vaddr->message = addr->message;
  vaddr->user_message = addr->user_message;
  vaddr->basic_errno = addr->basic_errno;
  vaddr->more_errno = addr->more_errno;
  vaddr->prop.address_data = addr->prop.address_data;
  vaddr->prop.variables = NULL;
  tree_dup((tree_node **)&vaddr->prop.variables, addr->prop.variables);
  copyflag(vaddr, addr, af_pass_message);
  }
return yield;
}




/**************************************************
* printf that automatically handles TLS if needed *
***************************************************/

/* This function is used by verify_address() as a substitute for all fprintf()
calls; a direct fprintf() will not produce output in a TLS SMTP session, such
as a response to an EXPN command.  smtp_in.c makes smtp_printf available but
that assumes that we always use the smtp_out_fd when not using TLS or the
ssl buffer when we are.  Instead we take an fd parameter and check to see if
that is smtp_out_fd; if so, smtp_printf() with TLS support, otherwise regular
dprintf().

Arguments:
  f           the candidate FILE* to write to
  format      format string
  ...         optional arguments

Returns:
              nothing
*/

static void PRINTF_FUNCTION(2,3)
respond_printf(int fd, const char * format, ...)
{
va_list ap;

va_start(ap, format);
if (smtp_out_fd >= 0 && fd == smtp_out_fd)
  smtp_vprintf(format, FALSE, ap);
else
  vdprintf(fd, format, ap);
va_end(ap);
}



/*************************************************
*            Verify an email address             *
*************************************************/

/* This function is used both for verification (-bv and at other times) and
address testing (-bt), which is indicated by address_test_mode being set.

Arguments:
  vaddr            contains the address to verify; the next field in this block
                     must be NULL
  fd               if >= 0, write the result to this file
  options          various option bits:
                     vopt_fake_sender => this sender verify is not for the real
                       sender (it was verify=sender=xxxx or an address from a
                       header line) - rewriting must not change sender_address
                     vopt_is_recipient => this is a recipient address, otherwise
                       it's a sender address - this affects qualification and
                       rewriting and messages from callouts
                     vopt_qualify => qualify an unqualified address; else error
                     vopt_expn => called from SMTP EXPN command
                     vopt_success_on_redirect => when a new address is generated
                       the verification instantly succeeds

                     These ones are used by do_callout() -- the options variable
                       is passed to it.

                     vopt_callout_fullpm => if postmaster check, do full one
                     vopt_callout_no_cache => don't use callout cache
                     vopt_callout_random   => do the "random" thing
                     vopt_callout_recipsender  => use real sender for recipient
                     vopt_callout_recippmaster => use postmaster for recipient
		     vopt_callout_r_tptsender  => use sender as defined by tpt

  callout          if > 0, specifies that callout is required, and gives timeout
                     for individual commands
  callout_overall  if > 0, gives overall timeout for the callout function;
                   if < 0, a default is used (see do_callout())
  callout_connect  the connection timeout for callouts
  se_mailfrom      when callout is requested to verify a sender, use this
                     in MAIL FROM; NULL => ""
  pm_mailfrom      when callout is requested, if non-NULL, do the postmaster
                     thing and use this as the sender address (may be "")

  routed           if not NULL, set TRUE if routing succeeded, so we can
                     distinguish between routing failed and callout failed

Returns:           OK      address verified
                   FAIL    address failed to verify
                   DEFER   can't tell at present
*/

int
verify_address(address_item * vaddr, int fd, int options, int callout,
  int callout_overall, int callout_connect, uschar * se_mailfrom,
  uschar * pm_mailfrom, BOOL * routed)
{
BOOL allok = TRUE;
BOOL full_info = fd >= 0 ? debug_selector != 0 : FALSE;
BOOL expn         = (options & vopt_expn) != 0;
BOOL success_on_redirect = (options & vopt_success_on_redirect) != 0;
int i;
int yield = OK;
int verify_type = expn ? v_expn :
   f.address_test_mode ? v_none :
          options & vopt_is_recipient ? v_recipient : v_sender;
address_item * addr_list, * addr_new = NULL, * addr_remote = NULL;
address_item * addr_local = NULL, * addr_succeed = NULL;
uschar ** failure_ptr = options & vopt_is_recipient
  ? &recipient_verify_failure : &sender_verify_failure;
uschar * ko_prefix, * cr;
const uschar * address = vaddr->address, * save_sender;
uschar null_sender[] = { 0 };             /* Ensure writeable memory */

/* Clear, just in case */

*failure_ptr = NULL;

/* Set up a prefix and suffix for error message which allow us to use the same
output statements both in EXPN mode (where an SMTP response is needed) and when
debugging with an output file. */

if (expn)
  {
  ko_prefix = US"553 ";
  cr = US"\r";
  }
else ko_prefix = cr = US"";

/* Add qualify domain if permitted; otherwise an unqualified address fails. */

if (parse_find_at(address) == NULL)
  {
  if (!(options & vopt_qualify))
    {
    if (fd >= 0)
      respond_printf(fd, "%sA domain is required for %q%s\n",
        ko_prefix, address, cr);
    *failure_ptr = US"qualify";
    return FAIL;
    }
  /* deconst ok as address was not const */
  address = US rewrite_address_qualify(address, options & vopt_is_recipient);
  }

DEBUG(D_verify)
  {
  debug_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
  debug_printf("%s %s\n", f.address_test_mode? "Testing" : "Verifying", address);
  }

/* Rewrite and report on it. Clear the domain and local part caches - these
may have been set by domains and local part tests during an ACL. */

if (global_rewrite_rules)
  {
  const uschar * old = address;
  address = rewrite_address(address, options & vopt_is_recipient, FALSE,
    global_rewrite_rules, rewrite_existflags);
  if (address != old)
    {
    for (int j = 0; j < (MAX_NAMED_LIST * 2)/32; j++) vaddr->localpart_cache[j] = 0;
    for (int j = 0; j < (MAX_NAMED_LIST * 2)/32; j++) vaddr->domain_cache[j] = 0;
    if (fd > 0 && !expn) dprintf(fd, "Address rewritten as: %s\n", address);
    }
  }

/* If this is the real sender address, we must update sender_address at
this point, because it may be referred to in the routers. */

if (!(options & (vopt_fake_sender|vopt_is_recipient)))
  sender_address = address;

/* If the address was rewritten to <> no verification can be done, and we have
to return OK. This rewriting is permitted only for sender addresses; for other
addresses, such rewriting fails. */

if (!address[0]) return OK;

/* Flip the legacy TLS-related variables over to the outbound set in case
they're used in the context of a transport used by verification. Reset them
at exit from this routine (so no returns allowed from here on). */

tls_modify_variables(&tls_out);

/* Save a copy of the sender address for re-instating if we change it to <>
while verifying a sender address (a nice bit of self-reference there). */

save_sender = sender_address;

/* Observability variable for router/transport use */

verify_mode = options & vopt_is_recipient ? US"R" : US"S";

/* Update the address structure with the possibly qualified and rewritten
address. Set it up as the starting address on the chain of new addresses. */

vaddr->address = address;
addr_new = vaddr;

/* We need a loop, because an address can generate new addresses. We must also
cope with generated pipes and files at the top level. (See also the code and
comment in deliver.c.) However, it is usually the case that the router for
user's .forward files has its verify flag turned off.

If an address generates more than one child, the loop is used only when
full_info is set, and this can only be set locally. Remote enquiries just get
information about the top level address, not anything that it generated. */

while (addr_new)
  {
  int rc;
  address_item *addr = addr_new;

  addr_new = addr->next;
  addr->next = NULL;

  DEBUG(D_verify)
    {
    debug_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
    debug_printf("Considering %s\n", addr->address);
    }

  /* Handle generated pipe, file or reply addresses. We don't get these
  when handling EXPN, as it does only one level of expansion. */

  if (testflag(addr, af_pfr))
    {
    allok = FALSE;
    if (fd > 0)
      {
      BOOL allow;

      if (addr->address[0] == '>')
        {
        allow = testflag(addr, af_allow_reply);
        dprintf(fd, "%s -> mail %s", addr->parent->address, addr->address + 1);
        }
      else
        {
        allow = addr->address[0] == '|'
          ? testflag(addr, af_allow_pipe) : testflag(addr, af_allow_file);
        dprintf(fd, "%s -> %s", addr->parent->address, addr->address);
        }

      if (addr->basic_errno == ERRNO_BADTRANSPORT)
        dprintf(fd, "\n*** Error in setting up pipe, file, or autoreply:\n"
          "%s\n", addr->message);
      else if (allow)
        dprintf(fd, "\n  transport = %s\n", addr->transport->drinst.name);
      else
        dprintf(fd, " *** forbidden ***\n");
      }
    continue;
    }

  /* Just in case some router parameter refers to it. */

  return_path = addr->prop.errors_address
    ? addr->prop.errors_address : sender_address;

  /* Split the address into domain and local part, handling the %-hack if
  necessary, and then route it. While routing a sender address, set
  $sender_address to <> because that is what it will be if we were trying to
  send a bounce to the sender. */

  if (routed) *routed = FALSE;
  if ((rc = deliver_split_address(addr)) == OK)
    {
    if (!(options & vopt_is_recipient)) sender_address = null_sender;
    rc = route_address(addr, &addr_local, &addr_remote, &addr_new,
      &addr_succeed, verify_type);
    sender_address = save_sender;     /* Put back the real sender */
    }

  /* If routing an address succeeded, set the flag that remembers, for use when
  an ACL cached a sender verify (in case a callout fails). Then if routing set
  up a list of hosts or the transport has a host list, and the callout option
  is set, and we aren't in a host checking run, do the callout verification,
  and set another flag that notes that a callout happened. */

  if (rc == OK)
    {
    if (routed) *routed = TRUE;
    if (callout > 0)
      {
      BOOL local_verify = FALSE;
      transport_instance * tp;
      host_item * host_list = addr->host_list;

      /* Make up some data for use in the case where there is no remote
      transport. */

      transport_feedback tf = {
        .interface =		NULL,                       /* interface (=> any) */
        .port =			US"25",
        .protocol =		NULL,
        .hosts =		NULL,
        .helo_data =		US"$smtp_active_hostname",
        .hosts_override =	FALSE,
        .hosts_randomize =	FALSE,
        .gethostbyname =	FALSE,
        .qualify_single =	TRUE,
        .search_parents =	FALSE
        };

      /* If verification yielded a remote transport, we want to use that
      transport's options, so as to mimic what would happen if we were really
      sending a message to this address. */

      if ((tp = addr->transport))
	{
	const uschar * save_deliver_domain = deliver_domain;
	const uschar * save_deliver_localpart = deliver_localpart;
	const transport_info * ti = tp->drinst.info;

	deliver_domain = addr->domain;
	deliver_localpart = addr->local_part;
	if (!ti->local)
	  {
	  if ((tp->setup)(tp, addr, &tf, 0, 0, NULL) != OK)
	    log_write(0, LOG_MAIN|LOG_PANIC,
	      "setup fail for %s transport for callout (%s)",
	      tp->drinst.name, expand_string_message);

	  /* If the transport has hosts and the router does not, or if the
	  transport is configured to override the router's hosts, we must build a
	  host list of the transport's hosts, and find the IP addresses */

	  if (tf.hosts && (!host_list || tf.hosts_override))
	    {
	    const uschar * s;

	    host_list = NULL;    /* Ignore the router's hosts */

	    if (!(s = expand_string(tf.hosts)))
	      log_write(0, LOG_MAIN|LOG_PANIC, "failed to expand list of hosts "
		"%q in %s transport for callout: %s", tf.hosts,
		tp->drinst.name, expand_string_message);
	    else
	      {
	      int flags;
	      host_build_hostlist(&host_list, s, tf.hosts_randomize);

	      /* Just ignore failures to find a host address. If we don't manage
	      to find any addresses, the callout will defer. Note that more than
	      one address may be found for a single host, which will result in
	      additional host items being inserted into the chain. Hence we must
	      save the next host first. */

	      flags = HOST_FIND_BY_A | HOST_FIND_BY_AAAA;
	      if (tf.qualify_single) flags |= HOST_FIND_QUALIFY_SINGLE;
	      if (tf.search_parents) flags |= HOST_FIND_SEARCH_PARENTS;

	      for (host_item * host = host_list, * nexthost; host; host = nexthost)
		{
		nexthost = host->next;
		if (tf.gethostbyname ||
		    string_is_ip_address(host->name, NULL) != 0)
		  (void)host_find_byname(host, NULL, flags, NULL, TRUE);
		else
		  {
		  const dnssec_domains * dsp = NULL;
		  if (Ustrcmp(tp->drinst.driver_name, "smtp") == 0)
		    {
		    smtp_transport_options_block * ob = tp->drinst.options_block;
		    dsp = &ob->dnssec;
		    }

		  (void) host_find_bydns(host, NULL, flags, NULL, NULL, NULL,
		    dsp, NULL, NULL);
		  }
		}
	      }
	    }
	  }
	else if (  options & vopt_quota
		&& Ustrcmp(tp->drinst.driver_name, "appendfile") == 0)
	  local_verify = TRUE;

	deliver_domain = save_deliver_domain;
	deliver_localpart = save_deliver_localpart;
	}

      /* Can only do a callout if we have at least one host! If the callout
      fails, it will have set ${sender,recipient}_verify_failure. */

      if (host_list)
        {
        HDEBUG(D_verify)
	  debug_printf("Attempting full verification using callout\n");
        if (host_checking && !f.host_checking_callout)
          {
          HDEBUG(D_verify)
            debug_printf("... callout omitted by default when host testing\n"
              "(Use -bhc if you want the callouts to happen.)\n");
          }
        else
          {
	  deliver_set_expansions(addr);
          rc = do_callout(addr, host_list, &tf, callout, callout_overall,
            callout_connect, options, se_mailfrom, pm_mailfrom);
	  deliver_set_expansions(NULL);

	  if (  options & vopt_is_recipient
	     && rc == OK
			 /* set to "random", with OK, for an accepted random */
	     && !recipient_verify_failure
	     )
	    callout_verified_rcpt(addr);
          }
        }
      else if (local_verify)
	{
        HDEBUG(D_verify) debug_printf("Attempting quota verification\n");

	deliver_set_expansions(addr);
	deliver_local(addr, TRUE);
	rc = addr->transport_return;
	}
      else
        HDEBUG(D_verify) debug_printf("Cannot do callout: neither router nor "
          "transport provided a host list, or transport is not smtp\n");
      }
    }

  /* Otherwise, any failure is a routing failure */

  else *failure_ptr = US"route";

  /* A router may return REROUTED if it has set up a child address as a result
  of a change of domain name (typically from widening). In this case we always
  want to continue to verify the new child. */

  if (rc == REROUTED) continue;

  /* Handle hard failures */

  if (rc == FAIL)
    {
    allok = FALSE;
    if (fd > 0)
      {
      address_item *p = addr->parent;

      respond_printf(fd, "%s%s %s", ko_prefix,
        full_info ? addr->address : address,
        f.address_test_mode ? "is undeliverable" : "failed to verify");
      if (!expn && f.admin_user)
        {
        if (addr->basic_errno > 0)
          respond_printf(fd, ": %s", strerror(addr->basic_errno));
        if (addr->message)
          respond_printf(fd, ": %s", addr->message);
        }

      /* Show parents iff doing full info */

      if (full_info) while (p)
        {
        respond_printf(fd, "%s\n    <-- %s", cr, p->address);
        p = p->parent;
        }
      respond_printf(fd, "%s\n", cr);
      }
    cancel_cutthrough_connection(TRUE, US"routing hard fail");

    if (!full_info)
      {
      yield = copy_error(vaddr, addr, FAIL);
      goto out;
      }
    yield = FAIL;
    }

  /* Soft failure */

  else if (rc == DEFER)
    {
    allok = FALSE;
    if (fd > 0)
      {
      address_item *p = addr->parent;
      respond_printf(fd, "%s%s cannot be resolved at this time", ko_prefix,
        full_info? addr->address : address);
      if (!expn && f.admin_user)
        {
        if (addr->basic_errno > 0)
          respond_printf(fd, ": %s", strerror(addr->basic_errno));
        if (addr->message)
          respond_printf(fd, ": %s", addr->message);
        else if (addr->basic_errno <= 0)
          respond_printf(fd, ": unknown error");
        }

      /* Show parents iff doing full info */

      if (full_info) while (p)
        {
        respond_printf(fd, "%s\n    <-- %s", cr, p->address);
        p = p->parent;
        }
      respond_printf(fd, "%s\n", cr);
      }
    cancel_cutthrough_connection(TRUE, US"routing soft fail");

    if (!full_info)
      {
      yield = copy_error(vaddr, addr, DEFER);
      goto out;
      }
    if (yield == OK) yield = DEFER;
    }

  /* If we are handling EXPN, we do not want to continue to route beyond
  the top level (whose address is in "address"). */

  else if (expn)
    {
    uschar *ok_prefix = US"250-";

    if (!addr_new)
      if (!addr_local && !addr_remote)
        respond_printf(fd, "250 mail to <%s> is discarded\r\n", address);
      else
        respond_printf(fd, "250 <%s>\r\n", address);

    else do
      {
      address_item *addr2 = addr_new;
      addr_new = addr2->next;
      if (!addr_new) ok_prefix = US"250 ";
      respond_printf(fd, "%s<%s>\r\n", ok_prefix, addr2->address);
      } while (addr_new);
    yield = OK;
    goto out;
    }

  /* Successful routing other than EXPN. */

  else
    {
    /* Handle successful routing when short info wanted. Otherwise continue for
    other (generated) addresses. Short info is the operational case. Full info
    can be requested only when debug_selector != 0 and a file is supplied.

    There is a conflict between the use of aliasing as an alternate email
    address, and as a sort of mailing list. If an alias turns the incoming
    address into just one address (e.g. J.Caesar->jc44) you may well want to
    carry on verifying the generated address to ensure it is valid when
    checking incoming mail. If aliasing generates multiple addresses, you
    probably don't want to do this. Exim therefore treats the generation of
    just a single new address as a special case, and continues on to verify the
    generated address. */

    if (  !full_info			/* Stop if short info wanted AND */
       && (  (  !addr_new		/* No new address OR */
             || addr_new->next		/* More than one new address OR */
	     || testflag(addr_new, af_pfr)	/* New address is pfr */
	     )
          ||				/* OR */
             (  addr_new		/* At least one new address AND */
             && success_on_redirect	/* success_on_redirect is set */
	  )  )
       )
      {
      if (fd > 0) dprintf(fd, "%s %s\n",
        address, f.address_test_mode ? "is deliverable" : "verified");

      /* If we have carried on to verify a child address, we want the value
      of $address_data to be that of the child */

      vaddr->prop.address_data = addr->prop.address_data;
      vaddr->prop.variables = NULL;
      tree_dup((tree_node **)&vaddr->prop.variables, addr->prop.variables);

      /* If stopped because more than one new address, cannot cutthrough */

      if (addr_new && addr_new->next)
	cancel_cutthrough_connection(TRUE, US"multiple addresses from routing");

      yield = OK;
      goto out;
      }
    }
  }     /* Loop for generated addresses */

/* Display the full results of the successful routing, including any generated
addresses. Control gets here only when full_info is set, which requires fd
valid, and this occurs only when a top-level verify is called with the
debugging switch on.

If there are no local and no remote addresses, and there were no pipes, files,
or autoreplies, and there were no errors or deferments, the message is to be
discarded, usually because of the use of :blackhole: in an alias file. */

if (allok && !addr_local && !addr_remote)
  {
  dprintf(fd, "mail to %s is discarded\n", address);
  goto out;
  }

for (addr_list = addr_local, i = 0; i < 2; addr_list = addr_remote, i++)
  while (addr_list)
    {
    address_item * addr = addr_list;
    transport_instance * tp = addr->transport;

    addr_list = addr->next;

    dprintf(fd, "%s", CS addr->address);

    /* If the address is a duplicate, show something about it. */

    if (!testflag(addr, af_pfr))
      if (tree_search(tree_duplicates, addr->unique))
        dprintf(fd, "   [duplicate, would not be delivered]");
      else
	tree_add_duplicate(addr->unique, addr);

    /* Now show its parents */

    for (address_item * p = addr->parent; p; p = p->parent)
      dprintf(fd, "\n    <-- %s", p->address);
    dprintf(fd, "\n  ");

    /* Show router, and transport */

    dprintf(fd, "router = %s, transport = %s\n",
      addr->router->drinst.name, tp ? tp->drinst.name : US"unset");

    /* Show any hosts that are set up by a router unless the transport
    is going to override them; fiddle a bit to get a nice format. */

    if (addr->host_list && tp && !tp->overrides_hosts)
      {
      const transport_info * ti = tp->drinst.info;
      int maxlen = 0, maxaddlen = 0;

      for (host_item * h = addr->host_list; h; h = h->next)
        {				/* get max lengths of host names, addrs */
        int len = Ustrlen(h->name);
        if (len > maxlen) maxlen = len;
        len = h->address ? Ustrlen(h->address) : 7;
        if (len > maxaddlen) maxaddlen = len;
        }
      for (host_item * h = addr->host_list; h; h = h->next)
	{
	dprintf(fd, "  host %-*s ", maxlen, h->name);

	if (h->address)
	  dprintf(fd, "[%s%-*c", h->address, maxaddlen+1 - Ustrlen(h->address), ']');
	else if (ti->local)
	  dprintf(fd, " %-*s ", maxaddlen, "");  /* Omit [unknown] for local */
	else
	  dprintf(fd, "[%s%-*c", "unknown", maxaddlen+1 - 7, ']');

        if (h->mx >= 0)
	  dprintf(fd, " MX=%d", h->mx);
        if (h->port != PORT_NONE)
	  dprintf(fd, " port=%d", h->port);
        if (f.running_in_test_harness  &&  h->dnssec_used == DS_YES)
	  write(fd, " AD", 3);
        if (h->status == hstatus_unusable)
	  dprintf(fd, " ** unusable **");
	write(fd, "\n", 1);
        }
      }
    }

/* Yield will be DEFER or FAIL if any one address has, only for full_info (which is
the -bv or -bt case). */

out:
verify_mode = NULL;
if (!(options & vopt_atrn))
  tls_modify_variables(&tls_in);	/* return variables to inbound values */

return yield;
}




/*************************************************
*      Check headers for syntax errors           *
*************************************************/

/* This function checks those header lines that contain addresses, and verifies
that all the addresses therein are 5322-syntactially correct.

Arguments:
  msgptr     where to put an error message

Returns:     OK
             FAIL
*/

int
verify_check_headers(uschar **msgptr)
{
uschar *colon, *s;
int yield = OK;

for (header_line * h = header_list; h && yield == OK; h = h->next)
  {
  if (h->type != htype_from &&
      h->type != htype_reply_to &&
      h->type != htype_sender &&
      h->type != htype_to &&
      h->type != htype_cc &&
      h->type != htype_bcc)
    continue;

  colon = Ustrchr(h->text, ':');
  s = colon + 1;
  Uskip_whitespace(&s);

  /* Loop for multiple addresses in the header, enabling group syntax. Note
  that we have to reset this after the header has been scanned. */

  f.parse_allow_group = TRUE;

  while (*s)
    {
    uschar * ss = parse_find_address_end(s, FALSE), * errmess;
    const uschar * recipient;
    uschar terminator = *ss;
    int start, end, domain;

    /* Temporarily terminate the string at this point, and extract the
    operative address within, allowing group syntax. */

    *ss = '\0';
    recipient = parse_extract_address(s,&errmess,&start,&end,&domain,FALSE);
    *ss = terminator;

    /* Permit an unqualified address only if the message is local, or if the
    sending host is configured to be permitted to send them. */

    if (recipient && !domain)
      {
      if (h->type == htype_from || h->type == htype_sender)
        {
        if (!f.allow_unqualified_sender) recipient = NULL;
        }
      else
        {
        if (!f.allow_unqualified_recipient) recipient = NULL;
        }
      if (!recipient) errmess = US"unqualified address not permitted";
      }

    /* It's an error if no address could be extracted, except for the special
    case of an empty address. */

    if (!recipient && Ustrcmp(errmess, "empty address") != 0)
      {
      uschar *verb = US"is";
      uschar *t = ss;
      uschar *tt = colon;
      int len;

      /* Arrange not to include any white space at the end in the
      error message or the header name. */

      while (t > s && isspace(t[-1])) t--;
      while (tt > h->text && isspace(tt[-1])) tt--;

      /* Add the address that failed to the error message, since in a
      header with very many addresses it is sometimes hard to spot
      which one is at fault. However, limit the amount of address to
      quote - cases have been seen where, for example, a missing double
      quote in a humungous To: header creates an "address" that is longer
      than string_sprintf can handle. */

      len = t - s;
      if (len > 1024)
        {
        len = 1024;
        verb = US"begins";
        }

      /* deconst cast ok as we're passing a non-const to string_printing() */
      *msgptr = US string_printing(
        string_sprintf("%s: failing address in \"%.*s:\" header %s: %.*s",
          errmess, (int)(tt - h->text), h->text, verb, len, s));

      yield = FAIL;
      break;          /* Out of address loop */
      }

    /* Advance to the next address */

    s = ss + (terminator ? 1 : 0);
    Uskip_whitespace(&s);
    }   /* Next address */

  f.parse_allow_group = FALSE;
  f.parse_found_group = FALSE;
  }     /* Next header unless yield has been set FALSE */

return yield;
}


/*************************************************
*      Check header names for 8-bit characters   *
*************************************************/

/* This function checks for invalid characters in header names. See
RFC 5322, 2.2. and RFC 6532, 3.

Arguments:
  msgptr     where to put an error message

Returns:     OK
             FAIL
*/

int
verify_check_header_names_ascii(uschar ** msgptr)
{

for (const header_line * h = header_list; h; h = h->next)
  for (const uschar * colon = Ustrchr(h->text, ':'), *s = h->text;
      s < colon; s++)
    if ((*s < 33) || (*s > 126))
      {
      *msgptr = string_sprintf("Invalid character in header \"%.*s\" found",
			     (int)(colon - h->text), h->text);
      return FAIL;
      }
return OK;
}

/*************************************************
*          Check for blind recipients            *
*************************************************/

/* This function checks that every (envelope) recipient is mentioned in either
the To: or Cc: header lines, thus detecting blind carbon copies.

There are two ways of scanning that could be used: either scan the header lines
and tick off the recipients, or scan the recipients and check the header lines.
The original proposed patch did the former, but I have chosen to do the latter,
because (a) it requires no memory and (b) will use fewer resources when there
are many addresses in To: and/or Cc: and only one or two envelope recipients.

Arguments:   case_sensitive   true if case sensitive matching should be used
Returns:     OK    if there are no blind recipients
             FAIL  if there is at least one blind recipient
*/

int
verify_check_notblind(BOOL case_sensitive)
{
for (int i = 0; i < recipients_count; i++)
  {
  BOOL found = FALSE;
  const uschar * address = recipients_list[i].address;

  for (header_line * h = header_list; !found && h; h = h->next)
    {
    uschar * colon, * s;

    if (h->type != htype_to && h->type != htype_cc) continue;

    colon = Ustrchr(h->text, ':');
    s = colon + 1;
    Uskip_whitespace(&s);

    /* Loop for multiple addresses in the header, enabling group syntax. Note
    that we have to reset this after the header has been scanned. */

    f.parse_allow_group = TRUE;

    while (*s)
      {
      uschar * ss = parse_find_address_end(s, FALSE), * errmess;
      const uschar * recipient;
      uschar terminator = *ss;
      int start, end, domain;

      /* Temporarily terminate the string at this point, and extract the
      operative address within, allowing group syntax. */

      *ss = '\0';
      recipient = parse_extract_address(s,&errmess,&start,&end,&domain,FALSE);
      *ss = terminator;

      /* If we found a valid recipient that has a domain, compare it with the
      envelope recipient. Local parts are compared with case-sensitivity
      according to the routine arg, domains case-insensitively.
      By comparing from the start with length "domain", we include the "@" at
      the end, which ensures that we are comparing the whole local part of each
      address. */

      if (recipient && domain != 0)
        if ((found = (case_sensitive
		? Ustrncmp(recipient, address, domain) == 0
		: strncmpic(recipient, address, domain) == 0)
	      && strcmpic(recipient + domain, address + domain) == 0))
	  break;

      /* Advance to the next address */

      s = ss + (terminator ? 1:0);
      Uskip_whitespace(&s);
      }   /* Next address */

    f.parse_allow_group = FALSE;
    f.parse_found_group = FALSE;
    }     /* Next header (if found is false) */

  if (!found) return FAIL;
  }       /* Next recipient */

return OK;
}



/*************************************************
*          Find if verified sender               *
*************************************************/

/* Usually, just a single address is verified as the sender of the message.
However, Exim can be made to verify other addresses as well (often related in
some way), and this is useful in some environments. There may therefore be a
chain of such addresses that have previously been tested. This function finds
whether a given address is on the chain.

Arguments:   the address to be verified
Returns:     pointer to an address item, or NULL
*/

address_item *
verify_checked_sender(const uschar * sender)
{
for (address_item * addr = sender_verified_list; addr; addr = addr->next)
  if (Ustrcmp(sender, addr->address) == 0) return addr;
return NULL;
}





/*************************************************
*             Get valid header address           *
*************************************************/

/* Scan the originator headers of the message, looking for an address that
verifies successfully. RFC 822 says:

    o   The "Sender" field mailbox should be sent  notices  of
        any  problems in transport or delivery of the original
        messages.  If there is no  "Sender"  field,  then  the
        "From" field mailbox should be used.

    o   If the "Reply-To" field exists, then the reply  should
        go to the addresses indicated in that field and not to
        the address(es) indicated in the "From" field.

So we check a Sender field if there is one, else a Reply_to field, else a From
field. As some strange messages may have more than one of these fields,
especially if they are resent- fields, check all of them if there is more than
one.

Arguments:
  user_msgptr      points to where to put a user error message
  log_msgptr       points to where to put a log error message
  callout          timeout for callout check (passed to verify_address())
  callout_overall  overall callout timeout (ditto)
  callout_connect  connect callout timeout (ditto)
  se_mailfrom      mailfrom for verify; NULL => ""
  pm_mailfrom      sender for pm callout check (passed to verify_address())
  options          callout options (passed to verify_address())
  verrno           where to put the address basic_errno

If log_msgptr is set to something without setting user_msgptr, the caller
normally uses log_msgptr for both things.

Returns:           result of the verification attempt: OK, FAIL, or DEFER;
                   FAIL is given if no appropriate headers are found
*/

int
verify_check_header_address(uschar ** user_msgptr, uschar ** log_msgptr,
  int callout, int callout_overall, int callout_connect, uschar * se_mailfrom,
  uschar * pm_mailfrom, int options, int * verrno)
{
static const int header_types[] = { htype_sender, htype_reply_to, htype_from };
BOOL done = FALSE;
int yield = FAIL;

for (int i = 0; i < 3 && !done; i++)
  for (const header_line * h = header_list; h && !done; h = h->next)
    {
    const uschar * endname, * s;
    uschar * ss;

    if (h->type != header_types[i]) continue;
    s = endname = Ustrchr(h->text, ':') + 1;

    /* Scan the addresses in the header, enabling group syntax. Note that we
    have to reset this after the header has been scanned. */

    f.parse_allow_group = TRUE;

    while (*s)
      {
      int terminator, new_ok;
      address_item * vaddr;

      while (isspace(*s) || *s == ',') s++;
      if (!*s) break;			/* End of header */

      ss = parse_find_address_end(s, FALSE);

      /* The terminator is a comma or end of header, but there may be white
      space preceding it (including newline for the last address). Move back
      past any white space so we can check against any cached envelope sender
      address verifications. */

      while (isspace(ss[-1])) ss--;
      terminator = *ss;
      *ss = '\0';

      HDEBUG(D_verify) debug_printf("verifying %.*s header address %s\n",
        (int)(endname - h->text), h->text, s);

      /* See if we have already verified this address as an envelope sender,
      and if so, use the previous answer. */

      vaddr = verify_checked_sender(s);

      if (vaddr != NULL &&                   /* Previously checked */
           (callout <= 0 ||                  /* No callout needed; OR */
            vaddr->special_action > 256))    /* Callout was done */
        {
        new_ok = vaddr->special_action & 255;
        HDEBUG(D_verify) debug_printf("previously checked as envelope sender\n");
        *ss = terminator;  /* Restore shortened string */
        }

      /* Otherwise we run the verification now. We must restore the shortened
      string before running the verification, so the headers are correct, in
      case there is any rewriting. */

      else
        {
        int start, end, domain;
        const uschar * address = parse_extract_address(s, log_msgptr,
						  &start, &end, &domain, FALSE);
        *ss = terminator;

        /* If we found an empty address, just carry on with the next one, but
        kill the message. */

        if (!address && Ustrcmp(*log_msgptr, "empty address") == 0)
          {
          *log_msgptr = NULL;
          s = ss;
          continue;
          }

        /* If verification failed because of a syntax error, fail this
        function, and ensure that the failing address gets added to the error
        message. */

        if (!address)
          {
          while (ss > s && isspace(ss[-1])) ss--;
          *log_msgptr = string_sprintf("syntax error in '%.*s' header when "
            "scanning for sender: %s in \"%.*s\"",
            (int)(endname - h->text), h->text, *log_msgptr, (int)(ss - s), s);
          yield = FAIL;
          done = TRUE;
          break;
          }

        /* Else go ahead with the sender verification. But it isn't *the*
        sender of the message, so set vopt_fake_sender to stop sender_address
        being replaced after rewriting or qualification. */

	vaddr = deliver_make_addr(address, FALSE);
	new_ok = verify_address(vaddr, -1, options | vopt_fake_sender,
	  callout, callout_overall, callout_connect, se_mailfrom,
	  pm_mailfrom, NULL);
        }

      /* We now have the result, either newly found, or cached. If we are
      giving out error details, set a specific user error. This means that the
      last of these will be returned to the user if all three fail. We do not
      set a log message - the generic one below will be used. */

      if (new_ok != OK)
        {
        *verrno = vaddr->basic_errno;
        if (smtp_return_error_details)
          *user_msgptr = string_sprintf("Rejected after DATA: "
            "could not verify \"%.*s\" header address\n%s: %s",
            (int)(endname - h->text), h->text, vaddr->address, vaddr->message);
        }

      /* Success or defer */

      if (new_ok == OK)
        {
        yield = OK;
        done = TRUE;
        break;
        }

      if (new_ok == DEFER) yield = DEFER;

      /* Move on to any more addresses in the header */

      s = ss;
      }     /* Next address */

    f.parse_allow_group = FALSE;
    f.parse_found_group = FALSE;
    }       /* Next header, unless done */
	    /* Next header type unless done */

if (yield == FAIL && *log_msgptr == NULL)
  *log_msgptr = US"there is no valid sender in any header line";

if (yield == DEFER && *log_msgptr == NULL)
  *log_msgptr = US"all attempts to verify a sender in a header line deferred";

return yield;
}




/*************************************************
*            Get RFC 1413 identification         *
*************************************************/

/* Attempt to get an id from the sending machine via the RFC 1413 protocol. If
the timeout is set to zero, then the query is not done. There may also be lists
of hosts and nets which are exempt. To guard against malefactors sending
non-printing characters which could, for example, disrupt a message's headers,
make sure the string consists of printing characters only.

Argument:
  port    the port to connect to; usually this is IDENT_PORT (113), but when
          running in the test harness with -bh a different value is used.

Returns:  nothing

Side effect: any received ident value is put in sender_ident (NULL otherwise)
*/

void
verify_get_ident(int port)
{
client_conn_ctx ident_conn_ctx = {0};
int host_af, qlen;
int received_sender_port, received_interface_port, n;
uschar *p;
blob early_data;
uschar buffer[2048];

/* Default is no ident. Check whether we want to do an ident check for this
host. */

sender_ident = NULL;
if (rfc1413_query_timeout <= 0 || verify_check_host(&rfc1413_hosts) != OK)
  return;

DEBUG(D_ident) debug_printf("doing ident callback\n");

/* Set up a connection to the ident port of the remote host. Bind the local end
to the incoming interface address. If the sender host address is an IPv6
address, the incoming interface address will also be IPv6. */

host_af = Ustrchr(sender_host_address, ':') == NULL ? AF_INET : AF_INET6;
if ((ident_conn_ctx.sock = ip_socket(SOCK_STREAM, host_af)) < 0) return;

if (ip_bind(ident_conn_ctx.sock, host_af, interface_address, 0) < 0)
  {
  DEBUG(D_ident) debug_printf("bind socket for ident failed: %s\n",
    strerror(errno));
  goto END_OFF;
  }

/* Construct and send the query. */

qlen = snprintf(CS buffer, sizeof(buffer), "%d , %d\r\n",
  sender_host_port, interface_port);
early_data.data = buffer;
early_data.len = qlen;

/*XXX we trust that the query is idempotent */
if (ip_connect(ident_conn_ctx.sock, host_af, sender_host_address, port,
		rfc1413_query_timeout, &early_data) < 0)
  {
  if (errno == ETIMEDOUT && LOGGING(ident_timeout))
    log_write(0, LOG_MAIN, "ident connection to %s timed out",
      sender_host_address);
  else
    DEBUG(D_ident) debug_printf("ident connection to %s failed: %s\n",
      sender_host_address, strerror(errno));
  goto END_OFF;
  }

/* Read a response line. We put it into the rest of the buffer, using several
recv() calls if necessary. */

p = buffer + qlen;

for (;;)
  {
  uschar *pp;
  int count;
  int size = sizeof(buffer) - (p - buffer);

  if (size <= 0) goto END_OFF;   /* Buffer filled without seeing \n. */
  count = ip_recv(&ident_conn_ctx, p, size, time(NULL) + rfc1413_query_timeout);
  if (count <= 0) goto END_OFF;  /* Read error or EOF */

  /* Scan what we just read, to see if we have reached the terminating \r\n. Be
  generous, and accept a plain \n terminator as well. The only illegal
  character is 0. */

  for (pp = p; pp < p + count; pp++)
    {
    if (*pp == 0) goto END_OFF;   /* Zero octet not allowed */
    if (*pp == '\n')
      {
      if (pp[-1] == '\r') pp--;
      *pp = 0;
      goto GOT_DATA;             /* Break out of both loops */
      }
    }

  /* Reached the end of the data without finding \n. Let the loop continue to
  read some more, if there is room. */

  p = pp;
  }

GOT_DATA:

/* We have received a line of data. Check it carefully. It must start with the
same two port numbers that we sent, followed by data as defined by the RFC. For
example,

  12345 , 25 : USERID : UNIX :root

However, the amount of white space may be different to what we sent. In the
"osname" field there may be several sub-fields, comma separated. The data we
actually want to save follows the third colon. Some systems put leading spaces
in it - we discard those. */

if (sscanf(CS buffer + qlen, "%d , %d%n", &received_sender_port,
      &received_interface_port, &n) != 2 ||
    received_sender_port != sender_host_port ||
    received_interface_port != interface_port)
  goto END_OFF;

p = buffer + qlen + n;
Uskip_whitespace(&p);
if (*p++ != ':') goto END_OFF;
Uskip_whitespace(&p);
if (Ustrncmp(p, "USERID", 6) != 0) goto END_OFF;
p += 6;
Uskip_whitespace(&p);
if (*p++ != ':') goto END_OFF;
while (*p && *p != ':') p++;
if (!*p++) goto END_OFF;
Uskip_whitespace(&p);
if (!*p) goto END_OFF;

/* The rest of the line is the data we want. We turn it into printing
characters when we save it, so that it cannot mess up the format of any logging
or Received: lines into which it gets inserted. We keep a maximum of 127
characters. The deconst cast is ok as we fed a nonconst to string_printing() */

sender_ident = US string_printing(string_copyn(p, 127));
DEBUG(D_ident) debug_printf("sender_ident = %s\n", sender_ident);

END_OFF:
(void)close(ident_conn_ctx.sock);
return;
}




/*************************************************
*      Match host to a single host-list item     *
*************************************************/

/* This function compares a host (name or address) against a single item
from a host list. The host name gets looked up if it is needed and is not
already known. The function is called from verify_check_this_host() via
match_check_list(), which is why most of its arguments are in a single block.

Arguments:
  arg            the argument block (see below)
  ss             the host-list item
  valueptr       where to pass back looked up data, or NULL
  error          for error message when returning ERROR

The block contains:
  host_name      (a) the host name, or
                 (b) NULL, implying use sender_host_name and
                       sender_host_aliases, looking them up if required, or
                 (c) the empty string, meaning that only IP address matches
                       are permitted
  host_address   the host address
  host_ipv4      the IPv4 address taken from an IPv6 one

Returns:         OK      matched
                 FAIL    did not match
                 DEFER   lookup deferred
                 ERROR   (a) failed to find the host name or IP address, or
                         (b) unknown lookup type specified, or
                         (c) host name encountered when only IP addresses are
                               being matched
*/

int
check_host(void * arg, const uschar * ss, const uschar ** valueptr, uschar ** error)
{
check_host_block * cb = (check_host_block *)arg;
int mlen = -1;
int maskoffset;
BOOL iplookup = FALSE, isquery = FALSE;
BOOL isiponly = cb->host_name && !cb->host_name[0];
const uschar * t;
uschar * semicolon, * opts;
const uschar * endname;
uschar ** aliases;

/* Optimize for the special case when the pattern is "*". */

if (*ss == '*' && !ss[1]) return OK;

/* If the pattern is empty, it matches only in the case when there is no host -
this can occur in ACL checking for SMTP input using the -bs option. In this
situation, the host address is the empty string. */

if (!cb->host_address[0]) return *ss ? FAIL : OK;
if (!*ss) return FAIL;

/* If the pattern is precisely "@" then match against the primary host name,
provided that host name matching is permitted; if it's "@[]" match against the
local host's IP addresses. */

if (*ss == '@')
  if (ss[1] == 0)
    {
    if (isiponly) return ERROR;
    ss = primary_hostname;
    }
  else if (Ustrcmp(ss, "@[]") == 0)
    {
    for (ip_address_item * ip = host_find_interfaces(); ip; ip = ip->next)
      if (Ustrcmp(ip->address, cb->host_address) == 0) return OK;
    return FAIL;
    }

/* If the pattern is an IP address, optionally followed by a bitmask count, do
a (possibly masked) comparison with the current IP address. */

if (string_is_ip_address(ss, &maskoffset) != 0)
  return host_is_in_net(cb->host_address, ss, maskoffset) ? OK : FAIL;

/* The pattern is not an IP address. A common error that people make is to omit
one component of an IPv4 address, either by accident, or believing that, for
example, 1.2.3/24 is the same as 1.2.3.0/24, or 1.2.3 is the same as 1.2.3.0,
which it isn't. (Those applications that do accept 1.2.3 as an IP address
interpret it as 1.2.0.3 because the final component becomes 16-bit - this is an
ancient specification.) To aid in debugging these cases, we give a specific
error if the pattern contains only digits and dots or contains a slash preceded
only by digits and dots (a slash at the start indicates a file name and of
course slashes may be present in lookups, but not preceded only by digits and
dots).  Then the equivalent for IPv6 (roughly). */

if (Ustrchr(ss, ':'))
  {
  for (t = ss; isxdigit(*t) || *t == ':' || *t == '.'; ) t++;
  if (!*t  ||  (*t == '/' || *t == '%') && t != ss)
    {
    *error = string_sprintf("malformed IPv6 address or address mask: %.*s", (int)(t - ss), ss);
    return ERROR;
    }
  }
else
  {
  for (t = ss; isdigit(*t) || *t == '.'; ) t++;
  if (!*t  || (*t == '/' && t != ss))
    {
    *error = string_sprintf("malformed IPv4 address or address mask: %.*s", (int)(t - ss), ss);
    return ERROR;
    }
  }

/* See if there is a semicolon in the pattern, separating a searchtype
prefix.  If there is one then check for comma-sep options. */

if ((semicolon = Ustrchr(ss, ';')))
  if ((opts = Ustrchr(ss, ',')) && opts < semicolon)
    {
    endname = opts++;
    opts = string_copyn(opts, semicolon - opts);
    }
  else
    {
    endname = semicolon;
    opts = NULL;
    }
else
  opts = NULL;

/* If we are doing an IP address only match, then all lookups must be IP
address lookups, even if there is no "net-". */

if (isiponly)
  iplookup = semicolon != NULL;

/* Otherwise, if the item is of the form net[n]-lookup;<file|query> then it is
a lookup on a masked IP network, in textual form. We obey this code even if we
have already set iplookup, so as to skip over the "net-" prefix and to set the
mask length. The net- stuff really only applies to single-key lookups where the
key is implicit. For query-style lookups the key is specified in the query.
From release 4.30, the use of net- for query style is no longer needed, but we
retain it for backward compatibility. */

if (Ustrncmp(ss, "net", 3) == 0 && semicolon)
  {
  mlen = 0;
  for (t = ss + 3; isdigit(*t); t++) mlen = mlen * 10 + *t - '0';
  if (mlen == 0 && t == ss+3) mlen = -1;  /* No mask supplied */
  iplookup = *t++ == '-';
  }
else
  t = ss;

/* Do the IP address lookup if that is indeed what we have */

if (iplookup)
  {
  const lookup_info * li;
  void * handle;
  const uschar * filename, * key, * result;
  uschar buffer[64];

  /* Find the search type */

  if (!(li = search_findtype(t, endname - t)))
    log_write_die(0, LOG_MAIN, "%s", search_error_message);

  /* Adjust parameters for the type of lookup. For a query-style lookup, there
  is no file name, and the "key" is just the query. For query-style with a file
  name, we have to fish the file off the start of the query. For a single-key
  lookup, the key is the current IP address, masked appropriately, and
  reconverted to text form, with the mask appended. For IPv6 addresses, specify
  dot separators instead of colons, except when the lookup type is "iplsearch".
  */

  if (mac_islookup(li, lookup_absfilequery))
    {
    filename = semicolon + 1;
    key = filename;
    Uskip_nonwhite(&key);
    filename = string_copyn(filename, key - filename);
    Uskip_whitespace(&key);
    }
  else if (mac_islookup(li, lookup_querystyle))
    {
    filename = NULL;
    key = semicolon + 1;
    }
  else   /* Single-key style */
    {
    int incoming[4], insize;
    int sep = Ustrcmp(li->name, "iplsearch") == 0 ? ':' : '.';
    insize = host_aton(cb->host_address, incoming);
    host_mask(insize, incoming, mlen);
    (void) host_nmtoa(insize, incoming, mlen, buffer, sep);
    key = buffer;
    filename = semicolon + 1;
    }

  /* Now do the actual lookup; note that there is no search_close() because
  of the caching arrangements. */

  if (!(handle = search_open(filename, li, 0, NULL, NULL)))
    log_write_die(0, LOG_MAIN, "%s", search_error_message);

  result = search_find(handle, filename, key, -1, NULL, 0, 0, NULL, opts);
  if (valueptr) *valueptr = result;
  return result ? OK : f.search_find_defer ? DEFER: FAIL;
  }

/* The pattern is not an IP address or network reference of any kind. That is,
it is a host name pattern. If this is an IP only match, there's an error in the
host list. */

if (isiponly)
  {
  *error = US"cannot match host name in match_ip list";
  return ERROR;
  }

/* Check the characters of the pattern to see if they comprise only letters,
digits, full stops, and hyphens (the constituents of domain names). Allow
underscores, as they are all too commonly found. Sigh. Also, if
allow_utf8_domains is set, allow top-bit characters. */

for (t = ss; *t; t++)
  if (!isalnum(*t) && *t != '.' && *t != '-' && *t != '_' &&
      (!allow_utf8_domains || *t < 128)) break;

/* If the pattern is a complete domain name, with no fancy characters, look up
its IP address and match against that. Note that a multi-homed host will add
items to the chain. */

if (!*t)
  {
  int rc;
  host_item h;
  h.next = NULL;
  h.name = ss;
  h.address = NULL;
  h.mx = MX_NONE;

  /* Using byname rather than bydns here means we cannot determine dnssec
  status.  On the other hand it is unclear how that could be either
  propagated up or enforced. */

  rc = host_find_byname(&h, NULL, HOST_FIND_QUALIFY_SINGLE, NULL, FALSE);
  if (rc == HOST_FOUND || rc == HOST_FOUND_LOCAL)
    {
    for (host_item * hh = &h; hh; hh = hh->next)
      if (host_is_in_net(hh->address, cb->host_address, 0)) return OK;
    return FAIL;
    }
  if (rc == HOST_FIND_AGAIN) return DEFER;
  *error = string_sprintf("failed to find IP address for %s", ss);
  return ERROR;
  }

/* Almost all subsequent comparisons require the host name, and can be done
using the general string matching function. When this function is called for
outgoing hosts, the name is always given explicitly. If it is NULL, it means we
must use sender_host_name and its aliases, looking them up if necessary. */

if (cb->host_name)   /* Explicit host name given */
  return match_check_string(cb->host_name, ss, -1,
    MCS_PARTIAL | MCS_CASELESS | MCS_AT_SPECIAL | cb->flags, valueptr);

/* Host name not given; in principle we need the sender host name and its
aliases. However, for query-style lookups, we do not need the name if the
query does not contain $sender_host_name. From release 4.23, a reference to
$sender_host_name causes it to be looked up, so we don't need to do the lookup
on spec. */

if ((semicolon = Ustrchr(ss, ';')))
  {
  const uschar * affix, * opts;
  int partial, affixlen, starflags;
  const lookup_info * li;

  *semicolon = 0;
  li = search_findtype_partial(ss, &partial, &affix, &affixlen, &starflags,
	  &opts);
  *semicolon=';';

  if (!li)				/* Unknown lookup type */
    {
    log_write(0, LOG_MAIN|LOG_PANIC, "%s in host list item %q",
      search_error_message, ss);
    return DEFER;
    }
  isquery = mac_islookup(li, lookup_querystyle|lookup_absfilequery);
  }

if (isquery)
  {
  switch(match_check_string(US"", ss, -1,
      MCS_PARTIAL| MCS_CASELESS| MCS_AT_SPECIAL | (cb->flags & MCS_CACHEABLE),
      valueptr))
    {
    case OK:    return OK;
    case DEFER: return DEFER;
    default:    return FAIL;
    }
  }

/* Not a query-style lookup; must ensure the host name is present, and then we
do a check on the name and all its aliases. */

if (!sender_host_name)
  {
  HDEBUG(D_host_lookup)
    debug_printf_indent("sender host name required, to match against %s\n", ss);
  expand_level++;
  if (host_lookup_failed || host_name_lookup() != OK)
    {
    expand_level--;
    *error = string_sprintf("failed to find host name for %s",
      sender_host_address);;
    return ERROR;
    }
  expand_level--;
  host_build_sender_fullhost();
  }

/* Match on the sender host name, using the general matching function */

switch(match_check_string(sender_host_name, ss, -1,
      MCS_PARTIAL| MCS_CASELESS| MCS_AT_SPECIAL | (cb->flags & MCS_CACHEABLE),
      valueptr))
  {
  case OK:    return OK;
  case DEFER: return DEFER;
  }

/* If there are aliases, try matching on them. */

aliases = sender_host_aliases;
while (*aliases)
  switch(match_check_string(*aliases++, ss, -1,
      MCS_PARTIAL| MCS_CASELESS| MCS_AT_SPECIAL | (cb->flags & MCS_CACHEABLE),
      valueptr))
    {
    case OK:    return OK;
    case DEFER: return DEFER;
    }
return FAIL;
}




/*************************************************
*    Check a specific host matches a host list   *
*************************************************/

/* This function is passed a host list containing items in a number of
different formats and the identity of a host. Its job is to determine whether
the given host is in the set of hosts defined by the list. The host name is
passed as a pointer so that it can be looked up if needed and not already
known. This is commonly the case when called from verify_check_host() to check
an incoming connection. When called from elsewhere the host name should usually
be set.

This function is now just a front end to match_check_list(), which runs common
code for scanning a list. We pass it the check_host() function to perform a
single test.

Arguments:
  listptr              pointer to the host list
  cache_bits           pointer to cache for named lists, or NULL
  host_name            the host name or NULL, implying use sender_host_name and
                         sender_host_aliases, looking them up if required
  host_address         the IP address
  valueptr             if not NULL, data from a lookup is passed back here

Returns:    OK    if the host is in the defined set
            FAIL  if the host is not in the defined set,
            DEFER if a data lookup deferred (not a host lookup)

If the host name was needed in order to make a comparison, and could not be
determined from the IP address, the result is FAIL unless the item
"+allow_unknown" was met earlier in the list, in which case OK is returned. */

int
verify_check_this_host(const uschar **listptr, unsigned int *cache_bits,
  const uschar *host_name, const uschar *host_address, const uschar **valueptr)
{
int rc;
unsigned int *local_cache_bits = cache_bits;
const uschar *save_host_address = deliver_host_address;
check_host_block cb = { .host_name = host_name, .host_address = host_address };

if (valueptr) *valueptr = NULL;

/* If the host address starts off ::ffff: it is an IPv6 address in
IPv4-compatible mode. Find the IPv4 part for checking against IPv4
addresses. */

cb.host_ipv4 = Ustrncmp(host_address, "::ffff:", 7) == 0
  ? host_address + 7 : host_address;

/* During the running of the check, put the IP address into $host_address. In
the case of calls from the smtp transport, it will already be there. However,
in other calls (e.g. when testing ignore_target_hosts), it won't. Just to be on
the safe side, any existing setting is preserved, though as I write this
(November 2004) I can't see any cases where it is actually needed. */

deliver_host_address = host_address;
rc = match_check_list(
       listptr,                                /* the list */
       0,                                      /* separator character */
       &hostlist_anchor,                       /* anchor pointer */
       &local_cache_bits,                      /* cache pointer */
       check_host,                             /* function for testing */
       &cb,                                    /* argument for function */
       MCL_HOST,                               /* type of check */
       host_address == sender_host_address
         ? US"host" : host_address,	       /* text for debugging */
       valueptr);                              /* where to pass back data */
deliver_host_address = save_host_address;
return rc;
}




/*************************************************
*      Check the given host item matches a list  *
*************************************************/
int
verify_check_given_host(const uschar ** listptr, const host_item * host)
{
return verify_check_this_host(listptr, NULL, host->name, host->address, NULL);
}

/*************************************************
*      Check the remote host matches a list      *
*************************************************/

/* This is a front end to verify_check_this_host(), created because checking
the remote host is a common occurrence. With luck, a good compiler will spot
the tail recursion and optimize it. If there's no host address, this is
command-line SMTP input - check against an empty string for the address.

Arguments:
  listptr              pointer to the host list

Returns:               the yield of verify_check_this_host(),
                       i.e. OK, FAIL, or DEFER
*/

int
verify_check_host(uschar **listptr)
{
return verify_check_this_host(CUSS listptr, sender_host_cache, NULL,
  sender_host_address ? sender_host_address : US"", NULL);
}





/*************************************************
*              Invert an IP address              *
*************************************************/

/* Originally just used for DNS xBL lists, now also used for the
reverse_ip expansion operator.

Arguments:
  buffer         where to put the answer
  address        the address to invert
*/

void
invert_address(uschar *buffer, uschar *address)
{
int bin[4];
uschar *bptr = buffer;

/* If this is an IPv4 address mapped into IPv6 format, adjust the pointer
to the IPv4 part only. */

if (Ustrncmp(address, "::ffff:", 7) == 0) address += 7;

/* Handle IPv4 address: when HAVE_IPV6 is false, the result of host_aton() is
always 1. */

if (host_aton(address, bin) == 1)
  {
  int x = bin[0];
  for (int i = 0; i < 4; i++)
    {
    sprintf(CS bptr, "%d.", x & 255);
    while (*bptr) bptr++;
    x >>= 8;
    }
  }

/* Handle IPv6 address. Actually, as far as I know, there are no IPv6 addresses
in any DNS black lists, and the format in which they will be looked up is
unknown. This is just a guess. */

#if HAVE_IPV6
else
  for (int j = 3; j >= 0; j--)
    {
    int x = bin[j];
    for (int i = 0; i < 8; i++)
      {
      sprintf(CS bptr, "%x.", x & 15);
      while (*bptr) bptr++;
      x >>= 4;
      }
    }
#endif

/* Remove trailing period -- this is needed so that both arbitrary
dnsbl keydomains and inverted addresses may be combined with the
same format string, "%s.%s" */

*(--bptr) = 0;
}



/****************************************************
  Verify a local user account for quota sufficiency
****************************************************/

/* The real work, done via a re-exec for privs, calls
down to the transport for the quota check.

Route and transport (in recipient-verify mode) the
given recipient. 

A routing result indicating any transport type other than appendfile
results in a fail.

Return, on stdout, a result string containing:
- highlevel result code (OK, DEFER, FAIL)
- errno
- where string
- message string
*/

void
verify_quota(uschar * address)
{
address_item vaddr = {.address = address};
BOOL routed;
uschar * msg = US"\0";
int rc, len = 1;

if ((rc = verify_address(&vaddr, -1, vopt_is_recipient | vopt_quota,
    1, 0, 0, NULL, NULL, &routed)) != OK)
  {
  uschar * where = recipient_verify_failure;
  msg = acl_verify_message ? acl_verify_message : vaddr.message;
  if (!msg) msg = US"";
  if (rc == DEFER && vaddr.basic_errno == ERRNO_EXIMQUOTA)
    {
    rc = FAIL;					/* DEFER -> FAIL */
    where = US"quota";
    vaddr.basic_errno = 0;
    }
  else if (!where) where = US"";

  len = 5 + Ustrlen(msg) + 1 + Ustrlen(where);
  msg = string_sprintf("%c%c%c%c%c%s%c%s", (uschar)rc,
    (vaddr.basic_errno >> 24) & 0xff, (vaddr.basic_errno >> 16) & 0xff,
    (vaddr.basic_errno >> 8) & 0xff, vaddr.basic_errno & 0xff,
    where, '\0', msg);
  }

DEBUG(D_verify) debug_printf_indent("verify_quota: len %d\n", len);
if (write(1, msg, len) != 0) ;
return;
}


/******************************************************************************/

/* Quota cache lookup.  We use the callout hints db also for the quota cache.
Return TRUE if a nonexpired record was found, having filled in the yield
argument.
*/

static BOOL
cached_quota_lookup(const uschar * rcpt, int * yield,
  int pos_cache, int neg_cache)
{
open_db dbblock, *dbm_file = NULL;
const dbdata_callout_cache_address * cache_address_record;

if (!pos_cache && !neg_cache)
  return FALSE;
if (!(dbm_file = dbfn_open(US"callout", O_RDWR|O_CREAT, &dbblock, FALSE, TRUE)))
  {
  HDEBUG(D_verify) debug_printf_indent("quota cache: not available\n");
  return FALSE;
  }
if (!(cache_address_record = (dbdata_callout_cache_address *)
    get_callout_cache_record(dbm_file, rcpt, US"address",
      pos_cache, neg_cache)))
  {
  dbfn_close(dbm_file);
  return FALSE;
  }
if (cache_address_record->result == ccache_accept)
  *yield = OK;
dbfn_close(dbm_file);
return TRUE;
}

/* Quota cache write */

static void
cache_quota_write(const uschar * rcpt, int yield, int pos_cache, int neg_cache)
{
open_db dbblock, *dbm_file = NULL;
dbdata_callout_cache_address cache_address_record;

if (!pos_cache && !neg_cache)
  return;
if (!(dbm_file = dbfn_open(US"callout", O_RDWR|O_CREAT, &dbblock, FALSE, TRUE)))
  {
  HDEBUG(D_verify) debug_printf_indent("quota cache: not available\n");
  return;
  }

cache_address_record.result = yield == OK ? ccache_accept : ccache_reject;

(void)dbfn_write(dbm_file, rcpt, &cache_address_record,
	(int)sizeof(dbdata_callout_cache_address));
HDEBUG(D_verify) debug_printf_indent("wrote %s quota cache record for %s\n",
      yield == OK ? "positive" : "negative", rcpt);

dbfn_close(dbm_file);
return;
}


/* To evaluate a local user's quota, starting in ACL, we need to
fork & exec to regain privileges, to that we can change to the user's
identity for access to their files.

Arguments:
 rcpt		Recipient account
 pos_cache	Number of seconds to cache a positive result (delivery
 		to be accepted).  Zero to disable caching.
 neg_cache	Number of seconds to cache a negative result.  Zero to disable.
 msg		Pointer to result string pointer

Return:		OK/DEFER/FAIL code
*/

int
verify_quota_call(const uschar * rcpt, int pos_cache, int neg_cache,
  uschar ** msg)
{
int pfd[2], pid, save_errno, yield = FAIL;
void (*oldsignal)(int);
const uschar * where = US"socketpair";

*msg = NULL;

if (cached_quota_lookup(rcpt, &yield, pos_cache, neg_cache))
  {
  HDEBUG(D_verify) debug_printf_indent("quota cache: address record is %s\n",
    yield == OK ? "positive" : "negative");
  if (yield != OK)
    {
    recipient_verify_failure = US"quota";
    acl_verify_message = *msg =
      US"Previous (cached) quota verification failure";
    }
  return yield;
  }

if (pipe(pfd) != 0)
  goto fail;

where = US"fork";
oldsignal = signal(SIGCHLD, SIG_DFL);
if ((pid = exim_fork(US"quota-verify")) < 0)
  {
  save_errno = errno;
  close(pfd[pipe_write]);
  close(pfd[pipe_read]);
  errno = save_errno;
  goto fail;
  }

if (pid == 0)		/* child */
  {
  close(pfd[pipe_read]);
  force_fd(pfd[pipe_write], 1);		/* stdout to pipe */
  close(pfd[pipe_write]);
  dup2(1, 0);
  if (debug_fd > 0) force_fd(debug_fd, 2);

  child_exec_exim(CEE_EXEC_EXIT, FALSE, NULL, FALSE, 3,
    US"-MCq", string_sprintf("%d", message_size), rcpt);
  /*NOTREACHED*/
  }

save_errno = errno;
close(pfd[pipe_write]);

if (pid < 0)
  {
  DEBUG(D_verify) debug_printf_indent(" fork: %s\n", strerror(save_errno));
  }
else
  {
  uschar buf[128];
  int n = read(pfd[pipe_read], buf, sizeof(buf));
  int status;

  waitpid(pid, &status, 0);
  if (status == 0)
    {
    if (n > 0) yield = buf[0];
    if (n > 4)
      save_errno = (buf[1] << 24) | (buf[2] << 16) | (buf[3] << 8) | buf[4];
    if ((recipient_verify_failure = n > 5
	? string_copyn_taint(buf+5, n-5, GET_UNTAINTED) : NULL))
      {
      const uschar * s = buf + 5 + Ustrlen(recipient_verify_failure) + 1;
      int m = n - (s - buf);
      acl_verify_message = *msg =
	m > 0 ? string_copyn_taint(s, m, GET_UNTAINTED) : NULL;
      }

    DEBUG(D_verify) debug_printf_indent("verify call response:"
      " len %d yield %s errno '%s' where '%s' msg '%s'\n",
      n, rc_names[yield], strerror(save_errno), recipient_verify_failure, *msg);

    if (  yield == OK
       || save_errno == 0 && Ustrcmp(recipient_verify_failure, "quota") == 0)
      cache_quota_write(rcpt, yield, pos_cache, neg_cache);
    else DEBUG(D_verify)
      debug_printf_indent("result not cacheable\n");
    }
  else
    {
    DEBUG(D_verify)
      debug_printf_indent("verify call response: waitpid status 0x%04x\n", status);
    }
  }

close(pfd[pipe_read]);
signal(SIGCHLD, oldsignal);
errno = save_errno;
return yield;

fail:
DEBUG(D_verify) debug_printf_indent("verify_quota_call fail in %s\n", where);
return yield;
}


/* vi: aw ai sw=2
*/
/* End of verify.c */
