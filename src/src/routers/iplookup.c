/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) The Exim Maintainers 2020 - 2024 */
/* Copyright (c) University of Cambridge 1995 - 2018 */
/* See the file NOTICE for conditions of use and distribution. */
/* SPDX-License-Identifier: GPL-2.0-or-later */


#include "../exim.h"

#ifdef ROUTER_IPLOOKUP		/* Remainder of file */
#include "rf_functions.h"
#include "iplookup.h"


/* IP connection types */

#define ip_udp 0
#define ip_tcp 1


/* Options specific to the iplookup router. */

optionlist iplookup_router_options[] = {
  { "hosts",    opt_stringptr,
      OPT_OFF(iplookup_router_options_block, hosts) },
  { "optional", opt_bool,
      OPT_OFF(iplookup_router_options_block, optional) },
  { "port",     opt_int,
      OPT_OFF(iplookup_router_options_block, port) },
  { "protocol", opt_stringptr,
      OPT_OFF(iplookup_router_options_block, protocol_name) },
  { "query",    opt_stringptr,
      OPT_OFF(iplookup_router_options_block, query) },
  { "reroute",  opt_stringptr,
      OPT_OFF(iplookup_router_options_block, reroute) },
  { "response_pattern", opt_stringptr,
      OPT_OFF(iplookup_router_options_block, response_pattern) },
  { "timeout",  opt_time,
      OPT_OFF(iplookup_router_options_block, timeout) }
};

/* Size of the options list. An extern variable has to be used so that its
address can appear in the tables drtables.c. */

int iplookup_router_options_count =
  sizeof(iplookup_router_options)/sizeof(optionlist);


#ifdef MACRO_PREDEF

/* Dummy entries */
iplookup_router_options_block iplookup_router_option_defaults = {0};
void iplookup_router_init(driver_instance *rblock) {}
int iplookup_router_entry(router_instance *rblock, address_item *addr,
  struct passwd *pw, int verify, address_item **addr_local,
  address_item **addr_remote, address_item **addr_new,
  address_item **addr_succeed) {return 0;}

#else   /*!MACRO_PREDEF*/


/* Default private options block for the iplookup router. */

iplookup_router_options_block iplookup_router_option_defaults = {
  -1,       /* port */
  ip_udp,   /* protocol */
  5,        /* timeout */
  NULL,     /* protocol_name */
  NULL,     /* hosts */
  NULL,     /* query; NULL => local_part@domain */
  NULL,     /* response_pattern; NULL => don't apply regex */
  NULL,     /* reroute; NULL => just used returned data */
  NULL,     /* re_response_pattern; compiled pattern */
  FALSE     /* optional */
};



/*************************************************
*          Initialization entry point            *
*************************************************/

/* Called for each instance, after its options have been read, to enable
consistency checks to be done, or anything else that needs to be set up. */

void
iplookup_router_init(driver_instance * r)
{
router_instance * rblock = (router_instance *)r;
iplookup_router_options_block * ob =
  (iplookup_router_options_block *) r->options_block;

/* A port and a host list must be given */

if (ob->port < 0)
  log_write_die(0, LOG_CONFIG_FOR, "%s router:\n  "
    "a port must be specified", r->name);

if (!ob->hosts)
  log_write_die(0, LOG_CONFIG_FOR, "%s router:\n  "
    "a host list must be specified", r->name);

/* Translate protocol name into value */

if (ob->protocol_name)
  {
  if (Ustrcmp(ob->protocol_name, "udp") == 0) ob->protocol = ip_udp;
  else if (Ustrcmp(ob->protocol_name, "tcp") == 0) ob->protocol = ip_tcp;
  else log_write_die(0, LOG_CONFIG_FOR, "%s router:\n  "
    "protocol not specified as udp or tcp", r->name);
  }

/* If a response pattern is given, compile it now to get the error early. */

if (ob->response_pattern)
  ob->re_response_pattern =
    regex_must_compile(ob->response_pattern, MCS_NOFLAGS, TRUE);
}



/*************************************************
*              Main entry point                  *
*************************************************/

/* See local README for interface details. This router returns:

DECLINE
  . pattern or identification match on returned data failed

DEFER
  . failed to expand the query or rerouting string
  . failed to create socket ("optional" not set)
  . failed to find a host, failed to connect, timed out ("optional" not set)
  . rerouting string is not in the form localpart@domain
  . verifying the errors address caused a deferment or a big disaster such
      as an expansion failure (rf_get_errors_address)
  . expanding a headers_{add,remove} string caused a deferment or another
      expansion error (rf_get_munge_headers)

PASS
  . failed to create socket ("optional" set)
  . failed to find a host, failed to connect, timed out ("optional" set)

OK
  . new address added to addr_new
*/

int
iplookup_router_entry(
  router_instance *rblock,        /* data for this instantiation */
  address_item *addr,             /* address we are working on */
  struct passwd *pw,              /* passwd entry after check_local_user */
  int verify,                     /* v_none/v_recipient/v_sender/v_expn */
  address_item **addr_local,      /* add it to this if it's local */
  address_item **addr_remote,     /* add it to this if it's remote */
  address_item **addr_new,        /* put new addresses on here */
  address_item **addr_succeed)    /* put old address here on success */
{
uschar *query = NULL;
uschar *reply;
uschar *hostname, *reroute, *domain;
const uschar *listptr;
uschar host_buffer[256];
host_item *host = store_get(sizeof(host_item), GET_UNTAINTED);
address_item *new_addr;
iplookup_router_options_block *ob =
  (iplookup_router_options_block *)(rblock->drinst.options_block);
const pcre2_code *re = ob->re_response_pattern;
int count, query_len, rc;
int sep = 0;

DEBUG(D_route) debug_printf_indent("%s router called for %s: domain = %s\n",
  rblock->drinst.name, addr->address, addr->domain);

reply = store_get(256, GET_TAINTED);

/* Build the query string to send. If not explicitly given, a default of
"user@domain user@domain" is used. */

GET_OPTION("query");
if (!ob->query)
  query = string_sprintf("%s@%s %s@%s", addr->local_part, addr->domain,
    addr->local_part, addr->domain);
else
  if (!(query = expand_string(ob->query)))
    {
    addr->message = string_sprintf("%s router: failed to expand %s: %s",
      rblock->drinst.name, ob->query, expand_string_message);
    return DEFER;
    }

query_len = Ustrlen(query);
DEBUG(D_route) debug_printf("%s router query is %q\n", rblock->drinst.name,
  string_printing(query));

/* Now connect to the required port for each of the hosts in turn, until a
response it received. Initialization insists on the port being set and there
being a host list. */

listptr = ob->hosts;
/* not expanded so should never be tainted */
while ((hostname = string_nextinlist(&listptr, &sep, host_buffer,
       sizeof(host_buffer))))
  {
  host_item *h;

  DEBUG(D_route) debug_printf("calling host %s\n", hostname);

  host->name = hostname;
  host->address = NULL;
  host->port = PORT_NONE;
  host->mx = MX_NONE;
  host->next = NULL;

  if (string_is_ip_address(host->name, NULL) != 0)
    host->address = host->name;
  else
    {
/*XXX might want dnssec request/require on an iplookup router? */
    int rc = host_find_byname(host, NULL, HOST_FIND_QUALIFY_SINGLE, NULL, TRUE);
    if (rc == HOST_FIND_FAILED || rc == HOST_FIND_AGAIN) continue;
    }

  /* Loop for possible multiple IP addresses for the given name. */

  for (h = host; h; h = h->next)
    {
    int host_af;
    client_conn_ctx query_cctx = {0};

    /* Skip any hosts for which we have no address */

    if (!h->address) continue;

    /* Create a socket, for UDP or TCP, as configured. IPv6 addresses are
    detected by checking for a colon in the address. */

    host_af = Ustrchr(h->address, ':') ? AF_INET6 : AF_INET;

    query_cctx.sock = ip_socket(ob->protocol == ip_udp ? SOCK_DGRAM:SOCK_STREAM,
      host_af);
    if (query_cctx.sock < 0)
      {
      if (ob->optional) return PASS;
      addr->message = string_sprintf("failed to create socket in %s router",
        rblock->drinst.name);
      return DEFER;
      }

    /* Connect to the remote host, under a timeout. In fact, timeouts can occur
    here only for TCP calls; for a UDP socket, "connect" always works (the
    router will timeout later on the read call). */
/*XXX could take advantage of TFO */

    if (ip_connect(query_cctx.sock, host_af, h->address,ob->port, ob->timeout,
		ob->protocol == ip_udp ? NULL : &tcp_fastopen_nodata) < 0)
      {
      close(query_cctx.sock);
      DEBUG(D_route)
        debug_printf("connection to %s failed: %s\n", h->address,
          strerror(errno));
      continue;
      }

    /* Send the query. If it fails, just continue with the next address. */

    if (send(query_cctx.sock, query, query_len, 0) < 0)
      {
      DEBUG(D_route) debug_printf("send to %s failed\n", h->address);
      (void)close(query_cctx.sock);
      continue;
      }

    /* Read the response and close the socket. If the read fails, try the
    next IP address. */

    count = ip_recv(&query_cctx, reply, sizeof(reply) - 1, time(NULL) + ob->timeout);
    (void)close(query_cctx.sock);
    if (count <= 0)
      {
      DEBUG(D_route) debug_printf("%s from %s\n", (errno == ETIMEDOUT)?
        "timed out" : "recv failed", h->address);
      *reply = 0;
      continue;
      }

    /* Success; break the loop */

    reply[count] = 0;
    DEBUG(D_route) debug_printf("%s router received %q from %s\n",
      rblock->drinst.name, string_printing(reply), h->address);
    break;
    }

  /* If h == NULL we have tried all the IP addresses and failed on all of them,
  so we must continue to try more host names. Otherwise we have succeeded. */

  if (h) break;
  }


/* If hostname is NULL, we have failed to find any host, or failed to
connect to any of the IP addresses, or timed out while reading or writing to
those we have connected to. In all cases, we must pass if optional and
defer otherwise. */

if (!hostname)
  {
  DEBUG(D_route) debug_printf("%s router failed to get anything\n", rblock->drinst.name);
  if (ob->optional) return PASS;
  addr->message = string_sprintf("%s router: failed to communicate with any "
    "host", rblock->drinst.name);
  return DEFER;
  }


/* If a response pattern was supplied, match the returned string against it. A
failure to match causes the router to decline. After a successful match, the
numerical variables for expanding the rerouted address are set up. */

if (re != NULL)
  {
  if (!regex_match_and_setup(re, reply, 0, -1))
    {
    DEBUG(D_route) debug_printf("%s router: %s failed to match response %s\n",
      rblock->drinst.name, ob->response_pattern, reply);
    return DECLINE;
    }
  }


/* If no response pattern was supplied, set up $0 as the response up to the
first white space (if any). Also, if no query was specified, check that what
follows the white space matches user@domain. */

else
  {
  int n = 0;
  while (reply[n] != 0 && !isspace(reply[n])) n++;
  expand_nmax = 0;
  expand_nstring[0] = reply;
  expand_nlength[0] = n;

  if (ob->query == NULL)
    {
    int nn = n;
    while (isspace(reply[nn])) nn++;
    if (Ustrcmp(query + query_len/2 + 1, reply+nn) != 0)
      {
      DEBUG(D_route) debug_printf("%s router: failed to match identification "
        "in response %s\n", rblock->drinst.name, reply);
      return DECLINE;
      }
    }

  reply[n] = 0;  /* Terminate for the default case */
  }

/* If an explicit rerouting string is specified, expand it. Otherwise, use
what was sent back verbatim. */

GET_OPTION("reroute");
if (ob->reroute)
  {
  reroute = expand_string(ob->reroute);
  expand_nmax = -1;
  if (!reroute)
    {
    addr->message = string_sprintf("%s router: failed to expand %s: %s",
      rblock->drinst.name, ob->reroute, expand_string_message);
    return DEFER;
    }
  }
else
  reroute = reply;

/* We should now have a new address in the form user@domain. */

if (!(domain = Ustrchr(reroute, '@')))
  {
  log_write(0, LOG_MAIN, "%s router: reroute string %s is not of the form "
    "user@domain", rblock->drinst.name, reroute);
  addr->message = string_sprintf("%s router: reroute string %s is not of the "
    "form user@domain", rblock->drinst.name, reroute);
  return DEFER;
  }

/* Create a child address with the old one as parent. Put the new address on
the chain of new addressess. */

new_addr = deliver_make_addr(reroute, TRUE);
new_addr->parent = addr;

new_addr->prop = addr->prop;

if (addr->child_count == USHRT_MAX)
  log_write_die(0, LOG_MAIN, "%s router generated more than %d "
    "child addresses for <%s>", rblock->drinst.name, USHRT_MAX, addr->address);
addr->child_count++;
new_addr->next = *addr_new;
*addr_new = new_addr;

/* Set up the errors address, if any, and the additional and removable headers
for this new address. */

rc = rf_get_errors_address(addr, rblock, verify, &new_addr->prop.errors_address);
if (rc != OK) return rc;

rc = rf_get_munge_headers(addr, rblock, &new_addr->prop.extra_headers,
  &new_addr->prop.remove_headers);
if (rc != OK) return rc;

return OK;
}



# ifdef DYNLOOKUP
#  define iplookup_router_info _router_info
# endif

router_info iplookup_router_info =
{
.drinfo = {
  .driver_name =	US"iplookup",
  .options =		iplookup_router_options,
  .options_count =	&iplookup_router_options_count,
  .options_block =	&iplookup_router_option_defaults,
  .options_len =	sizeof(iplookup_router_options_block),
  .init =		iplookup_router_init,
# ifdef DYNLOOKUP
  .dyn_magic =		ROUTER_MAGIC,
# endif
  },
.code =			iplookup_router_entry,
.tidyup =		NULL,     /* no tidyup entry */
.ri_flags =		ri_notransport
};

#endif	/*!MACRO_PREDEF*/
#endif	/*ROUTER_IPLOOKUP*/
/* End of routers/iplookup.c */
