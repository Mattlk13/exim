/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) The Exim Maintainers 2020 - 2025 */
/* Copyright (c) University of Cambridge 1995 - 2018 */
/* See the file NOTICE for conditions of use and distribution. */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Functions for finding hosts, either by gethostbyname(), gethostbyaddr(), or
directly via the DNS. When IPv6 is supported, getipnodebyname() and
getipnodebyaddr() may be used instead of gethostbyname() and gethostbyaddr(),
if the newer functions are available. This module also contains various other
functions concerned with hosts and addresses, and a random number function,
used for randomizing hosts with equal MXs but available for use in other parts
of Exim. */


#include "exim.h"


/* Static variable for preserving the list of interface addresses in case it is
used more than once. */

static ip_address_item *local_interface_data = NULL;


#ifdef USE_INET_NTOA_FIX
/*************************************************
*         Replacement for broken inet_ntoa()     *
*************************************************/

/* On IRIX systems, gcc uses a different structure passing convention to the
native libraries. This causes inet_ntoa() to always yield 0.0.0.0 or
255.255.255.255. To get round this, we provide a private version of the
function here. It is used only if USE_INET_NTOA_FIX is set, which should happen
only when gcc is in use on an IRIX system. Code send to me by J.T. Breitner,
with these comments:

  code by Stuart Levy
  as seen in comp.sys.sgi.admin

August 2005: Apparently this is also needed for AIX systems; USE_INET_NTOA_FIX
should now be set for them as well.

Arguments:  sa  an in_addr structure
Returns:        pointer to static text string
*/

char *
inet_ntoa(struct in_addr sa)
{
static uschar addr[20];
sprintf(addr, "%d.%d.%d.%d",
        (US &sa.s_addr)[0],
        (US &sa.s_addr)[1],
        (US &sa.s_addr)[2],
        (US &sa.s_addr)[3]);
  return addr;
}
#endif



/*************************************************
*              Random number generator           *
*************************************************/

/* This is a simple pseudo-random number generator. It does not have to be
very good for the uses to which it is put. When running the regression tests,
start with a fixed seed.

If you need better, see vaguely_random_number() which is potentially stronger,
if a crypto library is available, but might end up just calling this instead.

Arguments:
  limit:    one more than the largest number required

Returns:    a pseudo-random number in the range 0 to limit-1
*/

int
random_number(int limit)
{
if (limit < 1)
  return 0;
if (random_seed == 0)
  if (f.running_in_test_harness)
    random_seed = 42;
  else
    {
    int p = (int)getpid();
    random_seed = (int)time(NULL) ^ ((p << 16) | p);
    }
random_seed = 1103515245 * random_seed + 12345;
return (unsigned int)(random_seed >> 16) % limit;
}

/*************************************************
*      Wrappers for logging lookup times         *
*************************************************/

/* When the 'slow_lookup_log' variable is enabled, these wrappers will
write to the log file all (potential) dns lookups that take more than
slow_lookup_log milliseconds
*/

static void
log_long_lookup(const uschar * type, const uschar * data, unsigned long msec)
{
log_write(0, LOG_MAIN, "Long %s lookup for '%s': %lu msec",
  type, data, msec);
}


/* returns the current system epoch time in milliseconds. */
static unsigned long
get_time_in_ms()
{
struct timeval tmp_time;
unsigned long seconds, microseconds;

gettimeofday(&tmp_time, NULL);
seconds = (unsigned long) tmp_time.tv_sec;
microseconds = (unsigned long) tmp_time.tv_usec;
return seconds*1000 + microseconds/1000;
}


static int
dns_lookup_timerwrap(dns_answer *dnsa, const uschar *name, int type,
  const uschar **fully_qualified_name)
{
int retval;
unsigned long time_msec;

if (!slow_lookup_log)
  return dns_lookup(dnsa, name, type, fully_qualified_name);

time_msec = get_time_in_ms();
retval = dns_lookup(dnsa, name, type, fully_qualified_name);
if ((time_msec = get_time_in_ms() - time_msec) > slow_lookup_log)
  log_long_lookup(dns_text_type(type), name, time_msec);
return retval;
}


/*************************************************
*       Replace gethostbyname() when testing     *
*************************************************/

/* This function is called instead of gethostbyname(), gethostbyname2(), or
getipnodebyname() when running in the test harness. . It also
recognizes an unqualified "localhost" and forces it to the appropriate loopback
address. IP addresses are treated as literals. For other names, it uses the DNS
to find the host name. In the test harness, this means it will access only the
fake DNS resolver.

Arguments:
  name          the host name or a textual IP address
  af            AF_INET or AF_INET6
  error_num     where to put an error code:
                HOST_NOT_FOUND/TRY_AGAIN/NO_RECOVERY/NO_DATA

Returns:        a hostent structure or NULL for an error
*/

static struct hostent *
host_fake_gethostbyname(const uschar *name, int af, int *error_num)
{
#if HAVE_IPV6
int alen = (af == AF_INET)? sizeof(struct in_addr):sizeof(struct in6_addr);
#else
int alen = sizeof(struct in_addr);
#endif

int ipa;
const uschar *lname = name;
uschar *adds;
uschar **alist;
struct hostent *yield;
dns_answer * dnsa = store_get_dns_answer();
dns_scan dnss;

DEBUG(D_host_lookup)
  debug_printf_indent("using host_fake_gethostbyname for %s (%s)\n", name,
    af == AF_INET ? "IPv4" : "IPv6");

/* Handle unqualified "localhost" */

if (Ustrcmp(name, "localhost") == 0)
  lname = af == AF_INET ? US"127.0.0.1" : US"::1";

/* Handle a literal IP address */

if ((ipa = string_is_ip_address(lname, NULL)) != 0)
  if (   ipa == 4 && af == AF_INET
     ||  ipa == 6 && af == AF_INET6)
    {
    int x[4];
    yield = store_get(sizeof(struct hostent), GET_UNTAINTED);
    alist = store_get(2 * sizeof(char *), GET_UNTAINTED);
    adds  = store_get(alen, GET_UNTAINTED);
    yield->h_name = CS name;
    yield->h_aliases = NULL;
    yield->h_addrtype = af;
    yield->h_length = alen;
    yield->h_addr_list = CSS alist;
    *alist++ = adds;
    for (int n = host_aton(lname, x), i = 0; i < n; i++)
      {
      int y = x[i];
      *adds++ = (y >> 24) & 255;
      *adds++ = (y >> 16) & 255;
      *adds++ = (y >> 8) & 255;
      *adds++ = y & 255;
      }
    *alist = NULL;
    }

  /* Wrong kind of literal address */

  else
    {
    *error_num = HOST_NOT_FOUND;
    yield = NULL;
    goto out;
    }

/* Handle a host name */

else
  {
  int type = af == AF_INET ? T_A:T_AAAA;
  int rc = dns_lookup_timerwrap(dnsa, lname, type, NULL);
  int count = 0;

  lookup_dnssec_authenticated = NULL;

  switch(rc)
    {
    case DNS_SUCCEED: break;
    case DNS_NOMATCH: *error_num = HOST_NOT_FOUND; yield = NULL; goto out;
    case DNS_NODATA:  *error_num = NO_DATA; yield = NULL; goto out;
    case DNS_AGAIN:   *error_num = TRY_AGAIN; yield = NULL; goto out;
    default:
    case DNS_FAIL:    *error_num = NO_RECOVERY; yield = NULL; goto out;
    }

  for (dns_record * rr = dns_next_rr(dnsa, &dnss, RESET_ANSWERS);
       rr;
       rr = dns_next_rr(dnsa, &dnss, RESET_NEXT)) if (rr->type == type)
    count++;

  yield = store_get(sizeof(struct hostent), GET_UNTAINTED);
  alist = store_get((count + 1) * sizeof(char *), GET_UNTAINTED);
  adds  = store_get(count *alen, GET_UNTAINTED);

  yield->h_name = CS name;
  yield->h_aliases = NULL;
  yield->h_addrtype = af;
  yield->h_length = alen;
  yield->h_addr_list = CSS alist;

  for (dns_record * rr = dns_next_rr(dnsa, &dnss, RESET_ANSWERS);
       rr;
       rr = dns_next_rr(dnsa, &dnss, RESET_NEXT)) if (rr->type == type)
    {
    int x[4];
    dns_address *da;
    if (!(da = dns_address_from_rr(dnsa, rr))) break;
    *alist++ = adds;
    for (int n = host_aton(da->address, x), i = 0; i < n; i++)
      {
      int y = x[i];
      *adds++ = (y >> 24) & 255;
      *adds++ = (y >> 16) & 255;
      *adds++ = (y >> 8) & 255;
      *adds++ = y & 255;
      }
    }
  *alist = NULL;
  }

out:

store_free_dns_answer(dnsa);
return yield;
}



/*************************************************
*       Build chain of host items from list      *
*************************************************/

/* This function builds a chain of host items from a textual list of host
names. It does not do any lookups. If randomize is true, the chain is build in
a randomized order. There may be multiple groups of independently randomized
hosts; they are delimited by a host name consisting of just "+".

Arguments:
  anchor      anchor for the chain
  list        text list
  randomize   TRUE for randomizing

Returns:      nothing
*/

void
host_build_hostlist(host_item **anchor, const uschar *list, BOOL randomize)
{
int sep = 0;
int fake_mx = MX_NONE;          /* This value is actually -1 */
uschar *name;

if (!list) return;
if (randomize) fake_mx--;       /* Start at -2 for randomizing */

*anchor = NULL;

while ((name = string_nextinlist(&list, &sep, NULL, 0)))
  {
  host_item *h;

  if (name[0] == '+' && name[1] == 0)   /* "+" delimits a randomized group */
    {                                   /* ignore if not randomizing */
    if (randomize) fake_mx--;
    continue;
    }

  h = store_get(sizeof(host_item), GET_UNTAINTED);
  h->name = name;
  h->address = NULL;
  h->port = PORT_NONE;
  h->mx = fake_mx;
  h->sort_key = randomize ? (-fake_mx)*1000 + random_number(1000) : 0;
  h->status = hstatus_unknown;
  h->why = hwhy_unknown;
  h->dnssec_used = DS_UNK;
  h->last_try = 0;

  if (!*anchor)
    {
    h->next = NULL;
    *anchor = h;
    }
  else
    {
    host_item *hh = *anchor;
    if (h->sort_key < hh->sort_key)
      {
      h->next = hh;
      *anchor = h;
      }
    else
      {
      while (hh->next && h->sort_key >= hh->next->sort_key)
        hh = hh->next;
      h->next = hh->next;
      hh->next = h;
      }
    }
  }
}


/*************************************************
*         Get port from a host item's name       *
*************************************************/

/* This function is called when finding the IP address for a host that is in a
list of hosts explicitly configured, such as in the manualroute router, or in a
fallback hosts list. We see if there is a port specification at the end of the
host name, and if so, remove it. A minimum length of 3 is required for the
original name; nothing shorter is recognized as having a port.

We test for a name ending with a sequence of digits; if preceded by colon we
have a port if the character before the colon is ] and the name starts with [
or if there are no other colons in the name (i.e. it's not an IPv6 address).

Arguments:  pointer to the host item
Returns:    a port number or PORT_NONE
*/

int
host_item_get_port(host_item *h)
{
const uschar *p;
int port, x;
int len = Ustrlen(h->name);

if (len < 3 || (p = h->name + len - 1, !isdigit(*p))) return PORT_NONE;

/* Extract potential port number */

port = *p-- - '0';
x = 10;

while (p > h->name + 1 && isdigit(*p))
  {
  port += (*p-- - '0') * x;
  x *= 10;
  }

/* The smallest value of p at this point is h->name + 1. */

if (*p != ':') return PORT_NONE;

if (p[-1] == ']' && h->name[0] == '[')
  h->name = string_copyn(h->name + 1, p - h->name - 2);
else if (Ustrchr(h->name, ':') == p)
  h->name = string_copyn(h->name, p - h->name);
else return PORT_NONE;

DEBUG(D_route|D_host_lookup) debug_printf("host=%s port=%d\n", h->name, port);
return port;
}



#ifndef STAND_ALONE    /* Omit when standalone testing */

/*************************************************
*     Build sender_fullhost and sender_rcvhost   *
*************************************************/

/* This function is called when sender_host_name and/or sender_helo_name
have been set. Or might have been set - for a local message read off the spool
they won't be. In that case, do nothing. Otherwise, set up the fullhost string
as follows:

(a) No sender_host_name or sender_helo_name: "[ip address]"
(b) Just sender_host_name: "host_name [ip address]"
(c) Just sender_helo_name: "(helo_name) [ip address]" unless helo is IP
            in which case: "[ip address}"
(d) The two are identical: "host_name [ip address]" includes helo = IP
(e) The two are different: "host_name (helo_name) [ip address]"

If log_incoming_port is set, the sending host's port number is added to the IP
address.

This function also builds sender_rcvhost for use in Received: lines, whose
syntax is a bit different. This value also includes the RFC 1413 identity.
There wouldn't be two different variables if I had got all this right in the
first place.

Because this data may survive over more than one incoming SMTP message, it has
to be in permanent store.  However, STARTTLS has to be forgotten and redone
on a multi-message conn, so this will be called once per message then.  Hence
we use malloc, so we can free.

Arguments:  none
Returns:    nothing
*/

void
host_build_sender_fullhost(void)
{
BOOL show_helo = TRUE;
uschar * address, * fullhost, * rcvhost;
rmark reset_point;
int len;

if (!sender_host_address) return;

reset_point = store_mark();

/* Set up address, with or without the port. After discussion, it seems that
the only format that doesn't cause trouble is [aaaa]:pppp. However, we can't
use this directly as the first item for Received: because it ain't an RFC 2822
domain. Sigh. */

address = string_sprintf("[%s]:%d", sender_host_address, sender_host_port);
if (!LOGGING(incoming_port) || sender_host_port <= 0)
  *(Ustrrchr(address, ':')) = 0;

/* If there's no EHLO/HELO data, we can't show it. */

if (!sender_helo_name) show_helo = FALSE;

/* If HELO/EHLO was followed by an IP literal, it's messy because of two
features of IPv6. Firstly, there's the "IPv6:" prefix (Exim is liberal and
doesn't require this, for historical reasons). Secondly, IPv6 addresses may not
be given in canonical form, so we have to canonicalize them before comparing. As
it happens, the code works for both IPv4 and IPv6. */

else if (sender_helo_name[0] == '[' &&
         sender_helo_name[(len=Ustrlen(sender_helo_name))-1] == ']')
  {
  int offset = 1;
  uschar *helo_ip;

  if (strncmpic(sender_helo_name + 1, US"IPv6:", 5) == 0) offset += 5;
  if (strncmpic(sender_helo_name + 1, US"IPv4:", 5) == 0) offset += 5;

  helo_ip = string_copyn(sender_helo_name + offset, len - offset - 1);

  if (string_is_ip_address(helo_ip, NULL) != 0)
    {
    int x[4], y[4];
    int sizex, sizey;
    uschar ipx[48], ipy[48];    /* large enough for full IPv6 */

    sizex = host_aton(helo_ip, x);
    sizey = host_aton(sender_host_address, y);

    (void)host_nmtoa(sizex, x, -1, ipx, ':');
    (void)host_nmtoa(sizey, y, -1, ipy, ':');

    if (strcmpic(ipx, ipy) == 0) show_helo = FALSE;
    }
  }

/* Host name is not verified */

if (!sender_host_name)
  {
  uschar *portptr = Ustrstr(address, "]:");
  gstring * g;
  int adlen;    /* Sun compiler doesn't like ++ in initializers */

  adlen = portptr ? (++portptr - address) : Ustrlen(address);
  fullhost = sender_helo_name
    ? string_sprintf("(%s) %s", sender_helo_name, address)
    : address;

  g = string_catn(NULL, address, adlen);

  if (sender_ident || show_helo || portptr)
    {
    int firstptr;
    g = string_catn(g, US" (", 2);
    firstptr = g->ptr;

    if (portptr)
      g = string_append(g, 2, US"port=", portptr + 1);

    if (show_helo)
      g = string_append(g, 2,
        firstptr == g->ptr ? US"helo=" : US" helo=", sender_helo_name);

    if (sender_ident)
      g = string_append(g, 2,
        firstptr == g->ptr ? US"ident=" : US" ident=", sender_ident);

    g = string_catn(g, US")", 1);
    }

  rcvhost = string_from_gstring(g);
  }

/* Host name is known and verified. Unless we've already found that the HELO
data matches the IP address, compare it with the name. */

else
  {
  if (show_helo && strcmpic(sender_host_name, sender_helo_name) == 0)
    show_helo = FALSE;

  if (show_helo)
    {
    fullhost = string_sprintf("%s (%s) %s", sender_host_name,
      sender_helo_name, address);
    rcvhost = sender_ident
      ?  string_sprintf("%s\n\t(%s helo=%s ident=%s)", sender_host_name,
        address, sender_helo_name, sender_ident)
      : string_sprintf("%s (%s helo=%s)", sender_host_name,
        address, sender_helo_name);
    }
  else
    {
    fullhost = string_sprintf("%s %s", sender_host_name, address);
    rcvhost = sender_ident
      ?  string_sprintf("%s (%s ident=%s)", sender_host_name, address,
        sender_ident)
      : string_sprintf("%s (%s)", sender_host_name, address);
    }
  }

sender_fullhost = string_copy_perm(fullhost, TRUE);
sender_rcvhost = string_copy_perm(rcvhost, TRUE);

store_reset(reset_point);

DEBUG(D_host_lookup)
  {
  debug_printf_indent("sender_fullhost = %s\n", sender_fullhost);
  debug_printf_indent("sender_rcvhost = %s\n", sender_rcvhost);
  }
}



/*************************************************
*          Build host+ident message              *
*************************************************/

/* Used when logging rejections and various ACL and SMTP incidents. The text
return depends on whether sender_fullhost and sender_ident are set or not:

  no ident, no host   => U=unknown
  no ident, host set  => H=sender_fullhost
  ident set, no host  => U=ident
  ident set, host set => H=sender_fullhost U=ident

Arguments:
  useflag   TRUE if first item to be flagged (H= or U=); if there are two
              items, the second is always flagged

Returns:    pointer to an allocated string
*/

uschar *
host_and_ident(BOOL useflag)
{
gstring * g = NULL;

if (!sender_fullhost)
  {
  if (useflag)
    g = string_catn(g, US"U=", 2);
  g = string_cat(g, sender_ident ? sender_ident : US"unknown");
  }
else
  {
  if (useflag)
    g = string_catn(g, US"H=", 2);
  g = string_cat(g, sender_fullhost);
  if (LOGGING(incoming_interface) && interface_address)
    {
    g = string_fmt_append(g, " I=[%s]", interface_address);
    g = log_portnum(g, interface_port);
    }
  if (sender_ident)
    g = string_fmt_append(g, " U=%s", sender_ident);
  }
if (LOGGING(connection_id))
  g = string_fmt_append(g, " Ci=%s", connection_id);
gstring_release_unused(g);
return string_from_gstring(g);
}

#endif   /* STAND_ALONE */




/*************************************************
*         Build list of local interfaces         *
*************************************************/

/* This function interprets the contents of the local_interfaces or
extra_local_interfaces options, and creates an ip_address_item block for each
item on the list. There is no special interpretation of any IP addresses; in
particular, 0.0.0.0 and ::0 are returned without modification. If any address
includes a port, it is set in the block. Otherwise the port value is set to
zero.

Arguments:
  list        the list
  name        the name of the option being expanded

Returns:      a chain of ip_address_items, each containing to a textual
              version of an IP address, and a port number (host order) or
              zero if no port was given with the address
*/

ip_address_item *
host_build_ifacelist(const uschar *list, uschar *name)
{
int sep = 0;
uschar *s;
ip_address_item * yield = NULL, * last = NULL, * next;

while ((s = string_nextinlist(&list, &sep, NULL, 0)))
  {
  int ipv;
  int port = host_address_extract_port(s);            /* Leaves just the IP address */

  if (!(ipv = string_is_ip_address(s, NULL)))
    log_write_die(0, LOG_MAIN, "Malformed IP address %q in %s",
      s, name);

  /* Skip IPv6 addresses if IPv6 is disabled. */

  if (disable_ipv6 && ipv == 6) continue;

  /* This use of strcpy() is OK because we have checked that s is a valid IP
  address above. The field in the ip_address_item is large enough to hold an
  IPv6 address. */

  next = store_get(sizeof(ip_address_item), list);
  next->next = NULL;
  Ustrcpy(next->address, s);
  next->port = port;
  next->v6_include_v4 = FALSE;
  next->log = NULL;

  if (!yield)
    yield = last = next;
  else
    {
    last->next = next;
    last = next;
    }
  }

return yield;
}





/*************************************************
*         Find addresses on local interfaces     *
*************************************************/

/* This function finds the addresses of local IP interfaces. These are used
when testing for routing to the local host. As the function may be called more
than once, the list is preserved in permanent store, pointed to by a static
variable, to save doing the work more than once per process.

The generic list of interfaces is obtained by calling host_build_ifacelist()
for local_interfaces and extra_local_interfaces. This list scanned to remove
duplicates (which may exist with different ports - not relevant here). If
either of the wildcard IP addresses (0.0.0.0 and ::0) are encountered, they are
replaced by the appropriate (IPv4 or IPv6) list of actual local interfaces,
obtained from os_find_running_interfaces().

Arguments:    none
Returns:      a chain of ip_address_items, each containing to a textual
              version of an IP address; the port numbers are not relevant
*/


/* First, a local subfunction to add an interface to a list in permanent store,
but only if there isn't a previous copy of that address on the list. */

static ip_address_item *
add_unique_interface(ip_address_item *list, ip_address_item *ipa)
{
ip_address_item *ipa2;
for (ipa2 = list; ipa2; ipa2 = ipa2->next)
  if (Ustrcmp(ipa2->address, ipa->address) == 0) return list;
ipa2 = store_get_perm(sizeof(ip_address_item), GET_UNTAINTED);
*ipa2 = *ipa;
ipa2->next = list;
return ipa2;
}


/* This is the globally visible function */

ip_address_item *
host_find_interfaces(void)
{
ip_address_item *running_interfaces = NULL;

if (!local_interface_data)
  {
  void *reset_item = store_mark();
  ip_address_item *dlist = host_build_ifacelist(CUS local_interfaces,
    US"local_interfaces");
  ip_address_item *xlist = host_build_ifacelist(CUS extra_local_interfaces,
    US"extra_local_interfaces");
  ip_address_item *ipa;

  if (!dlist) dlist = xlist;
  else
    {
    for (ipa = dlist; ipa->next; ipa = ipa->next) ;
    ipa->next = xlist;
    }

  for (ipa = dlist; ipa; ipa = ipa->next)
    {
    if (Ustrcmp(ipa->address, "0.0.0.0") == 0 ||
        Ustrcmp(ipa->address, "::0") == 0)
      {
      BOOL ipv6 = ipa->address[0] == ':';
      if (!running_interfaces)
        running_interfaces = os_find_running_interfaces();
      for (ip_address_item * ipa2 = running_interfaces; ipa2; ipa2 = ipa2->next)
        if ((Ustrchr(ipa2->address, ':') != NULL) == ipv6)
          local_interface_data = add_unique_interface(local_interface_data,
						      ipa2);
      }
    else
      {
      local_interface_data = add_unique_interface(local_interface_data, ipa);
      DEBUG(D_interface)
        {
        debug_printf("Configured local interface: address=%s", ipa->address);
        if (ipa->port != 0) debug_printf(" port=%d", ipa->port);
        debug_printf("\n");
        }
      }
    }
  store_reset(reset_item);
  }

return local_interface_data;
}





/*************************************************
*        Convert network IP address to text      *
*************************************************/

/* Given an IPv4 or IPv6 address in binary, convert it to a text
string and return the result in a piece of new store. The address can
either be given directly, or passed over in a sockaddr structure. Note
that this isn't the converse of host_aton() because of byte ordering
differences. See host_nmtoa() below.

Arguments:
  type       if < 0 then arg points to a sockaddr, else
             either AF_INET or AF_INET6
  arg        points to a sockaddr if type is < 0, or
             points to an IPv4 address (32 bits), or
             points to an IPv6 address (128 bits),
             in both cases, in network byte order
  buffer     if NULL, the result is returned in gotten store;
             else points to a buffer to hold the answer
  portptr    points to where to put the port number, if non NULL; only
             used when type < 0

Returns:     pointer to character string
*/

uschar *
host_ntoa(int type, const void * arg, uschar * buffer, int * portptr)
{
uschar * yield;

/* The new world. It is annoying that we have to fish out the address from
different places in the block, depending on what kind of address it is. It
is also a pain that inet_ntop() returns a const uschar *, whereas the IPv4
function inet_ntoa() returns just uschar *, and some picky compilers insist
on warning if one assigns a const uschar * to a uschar *. Hence the casts. */

#if HAVE_IPV6
uschar addr_buffer[46];
if (type < 0)
  {
  int family = ((struct sockaddr *)arg)->sa_family;
  if (family == AF_INET6)
    {
    struct sockaddr_in6 *sk = (struct sockaddr_in6 *)arg;
    yield = US inet_ntop(family, &(sk->sin6_addr), CS addr_buffer,
      sizeof(addr_buffer));
    if (portptr) *portptr = ntohs(sk->sin6_port);
    }
  else
    {
    struct sockaddr_in *sk = (struct sockaddr_in *)arg;
    yield = US inet_ntop(family, &(sk->sin_addr), CS addr_buffer,
      sizeof(addr_buffer));
    if (portptr) *portptr = ntohs(sk->sin_port);
    }
  }
else
  {
  yield = US inet_ntop(type, arg, CS addr_buffer, sizeof(addr_buffer));
  }

/* If the result is a mapped IPv4 address, show it in V4 format. */

if (Ustrncmp(yield, "::ffff:", 7) == 0) yield += 7;

#else  /* HAVE_IPV6 */

/* The old world */

if (type < 0)
  {
  yield = US inet_ntoa(((struct sockaddr_in *)arg)->sin_addr);
  if (portptr) *portptr = ntohs(((struct sockaddr_in *)arg)->sin_port);
  }
else
  yield = US inet_ntoa(*((struct in_addr *)arg));
#endif

/* If there is no buffer, put the string into some new store. */

if (!buffer) buffer = store_get(46, GET_UNTAINTED);

/* Callers of this function with a non-NULL buffer must ensure that it is
large enough to hold an IPv6 address, namely, at least 46 bytes. That's what
makes this use of strcpy() OK.
If the library returned apparently an apparently tainted string, clean it;
we trust IP addresses. */

string_format_nt(buffer, 46, "%s", yield);
return buffer;
}




/*************************************************
*         Convert address text to binary         *
*************************************************/

/* Given the textual form of an IP address, convert it to binary in an
array of ints. IPv4 addresses occupy one int; IPv6 addresses occupy 4 ints.
The result has the first byte in the most significant byte of the first int. In
other words, the result is not in network byte order, but in host byte order.
As a result, this is not the converse of host_ntoa(), which expects network
byte order. See host_nmtoa() below.

Arguments:
  address    points to the textual address, checked for syntax
  bin        points to an array of 4 ints

Returns:     the number of ints used
*/

int
host_aton(const uschar * address, int * bin)
{
int x[4];
int v4offset = 0;

/* Handle IPv6 address, which may end with an IPv4 address. It may also end
with a "scope", introduced by a percent sign. This code is NOT enclosed in #if
HAVE_IPV6 in order that IPv6 addresses are recognized even if IPv6 is not
supported. */

if (Ustrchr(address, ':') != NULL)
  {
  const uschar * p = address;
  const uschar * component[8];
  BOOL ipv4_ends = FALSE;
  int ci = 0, nulloffset = 0, v6count = 8, i;

  /* If the address starts with a colon, it will start with two colons.
  Just lose the first one, which will leave a null first component. */

  if (*p == ':') p++;

  /* Split the address into components separated by colons. The input address
  is supposed to be checked for syntax. There was a case where this was
  overlooked; to guard against that happening again, check here and crash if
  there are too many components. */

  while (*p && *p != '%')
    {
    int len = Ustrcspn(p, ":%");
    if (len == 0) nulloffset = ci;
    if (ci > 7) log_write_die(0, LOG_MAIN,
      "Internal error: invalid IPv6 address %q passed to host_aton()",
      address);
    else component[ci++] = p;
    p += len;
    if (*p == ':') p++;
    }

  /* If the final component contains a dot, it is a trailing v4 address.
  As the syntax is known to be checked, just set up for a trailing
  v4 address and restrict the v6 part to 6 components. */

  if (Ustrchr(component[ci-1], '.') != NULL)
    {
    address = component[--ci];
    ipv4_ends = TRUE;
    v4offset = 3;
    v6count = 6;
    }

  /* If there are fewer than 6 or 8 components, we have to insert some
  more empty ones in the middle. */

  if (ci < v6count)
    {
    int insert_count = v6count - ci;
    for (i = v6count-1; i > nulloffset + insert_count; i--)
      component[i] = component[i - insert_count];
    while (i > nulloffset) component[i--] = US"";
    }

  /* Now turn the components into binary in pairs and bung them
  into the vector of ints. */

  for (i = 0; i < v6count; i += 2)
    bin[i/2] = (Ustrtol(component[i], NULL, 16) << 16) +
      Ustrtol(component[i+1], NULL, 16);

  /* If there was no terminating v4 component, we are done. */

  if (!ipv4_ends) return 4;
  }

/* Handle IPv4 address */

(void)sscanf(CS address, "%d.%d.%d.%d", x, x+1, x+2, x+3);
bin[v4offset] = ((uint)x[0] << 24) + (x[1] << 16) + (x[2] << 8) + x[3];
return v4offset+1;
}


/*************************************************
*           Apply mask to an IP address          *
*************************************************/

/* Mask an address held in 1 or 4 ints, with the ms bit in the ms bit of the
first int, etc.

Arguments:
  count        the number of ints
  binary       points to the ints to be masked
  mask         the count of ms bits to leave, or -1 if no masking

Returns:       nothing
*/

void
host_mask(int count, int *binary, int mask)
{
if (mask < 0) mask = 99999;
for (int i = 0; i < count; i++)
  {
  int wordmask;
  if (mask == 0) wordmask = 0;
  else if (mask < 32)
    {
    wordmask = (uint)(-1) << (32 - mask);
    mask = 0;
    }
  else
    {
    wordmask = -1;
    mask -= 32;
    }
  binary[i] &= wordmask;
  }
}




/*************************************************
*     Convert masked IP address in ints to text  *
*************************************************/

/* We can't use host_ntoa() because it assumes the binary values are in network
byte order, and these are the result of host_aton(), which puts them in ints in
host byte order. Also, we really want IPv6 addresses to be in a canonical
format, so we output them with no abbreviation. In a number of cases we can't
use the normal colon separator in them because it terminates keys in lsearch
files, so we want to use dot instead. There's an argument that specifies what
to use for IPv6 addresses.

Arguments:
  count       1 or 4 (number of ints)
  binary      points to the ints
  mask        mask value; if < 0 don't add to result
  buffer      big enough to hold the result
  sep         component separator character for IPv6 addresses

Returns:      the number of characters placed in buffer, not counting
              the final nul.
*/

int
host_nmtoa(int count, const int * binary, int mask, uschar * buffer, int sep)
{
uschar * tt = buffer;

if (count == 1)
  for (int j = binary[0], i = 24; i >= 0; i -= 8)
    tt += sprintf(CS tt, "%d.", (j >> i) & 255);
else
  for (int j, i = 0; i < 4; i++)
    {
    j = binary[i];
    tt += sprintf(CS tt, "%04x%c%04x%c", (j >> 16) & 0xffff, sep, j & 0xffff, sep);
    }

tt--;   /* lose final separator */

if (mask < 0)
  *tt = 0;
else
  tt += sprintf(CS tt, "/%d", mask);

return tt - buffer;
}


/* Like host_nmtoa() but: ipv6-only, canonical output, no mask

Arguments:
  binary      points to the ints
  buffer      big enough to hold the result

Returns:      the number of characters placed in buffer, not counting
	      the final nul.
*/

int
ipv6_nmtoa(int * binary, uschar * buffer)
{
int i, j, k;
uschar * c = buffer;
uschar * d = NULL;	/* shut insufficiently "clever" compiler up */

for (i = 0; i < 4; i++)
  {			/* expand to text */
  j = binary[i];
  c += sprintf(CS c, "%x:%x:", (j >> 16) & 0xffff, j & 0xffff);
  }

for (c = buffer, k = -1, i = 0; i < 8; i++)
  {			/* find longest 0-group sequence */
  if (*c == '0')	/* must be "0:" */
    {
    uschar * s = c;
    j = i;
    while (c[2] == '0') i++, c += 2;
    if (i-j > k)
      {
      k = i-j;		/* length of sequence */
      d = s;		/* start of sequence */
      }
    }
  while (*++c != ':') ;
  c++;
  }

*--c = '\0';	/* drop trailing colon */

/* debug_printf("%s: D k %d <%s> <%s>\n", __FUNCTION__, k, buffer, buffer + 2*(k+1)); */
if (k >= 0)
  {			/* collapse */
  c = d + 2*(k+1);
  if (d == buffer) c--;	/* need extra colon */
  *d++ = ':';	/* 1st 0 */
  while ((*d++ = *c++)) ;
  }
else
  d = c;

return d - buffer;
}



/*************************************************
*        Check port for tls_on_connect           *
*************************************************/

/* This function checks whether a given incoming port is configured for tls-
on-connect. It is called from the daemon and from inetd handling. If the global
option tls_on_connect is already set, all ports operate this way. Otherwise, we
check the tls_on_connect_ports option for a list of ports.

Argument:  a port number
Returns:   TRUE or FALSE
*/

BOOL
host_is_tls_on_connect_port(int port)
{
#ifndef DISABLE_TLS
int sep = 0;
const uschar * list = tls_on_connect_ports;

if (f.tls_on_connect) return TRUE;

for (uschar * s, * end; s = string_nextinlist(&list, &sep, NULL, 0); )
  if (Ustrtol(s, &end, 10) == port)
    return TRUE;
#endif

return FALSE;
}



/*************************************************
*        Check whether host is in a network      *
*************************************************/

/* This function checks whether a given IP address matches a pattern that
represents either a single host, or a network (using CIDR notation). The caller
of this function must check the syntax of the arguments before calling it.

Arguments:
  host        string representation of the ip-address to check
  net         string representation of the network, with optional CIDR mask
  maskoffset  offset to the / that introduces the mask in the key
              zero if there is no mask

Returns:
  TRUE   the host is inside the network
  FALSE  the host is NOT inside the network
*/

BOOL
host_is_in_net(const uschar *host, const uschar *net, int maskoffset)
{
int address[4];
int incoming[4];
int mlen;
int size = host_aton(net, address);
int insize;

/* No mask => all bits to be checked */

if (maskoffset == 0) mlen = 99999;    /* Big number */
  else mlen = Uatoi(net + maskoffset + 1);

/* Convert the incoming address to binary. */

insize = host_aton(host, incoming);

/* Convert IPv4 addresses given in IPv6 compatible mode, which represent
   connections from IPv4 hosts to IPv6 hosts, that is, addresses of the form
   ::ffff:<v4address>, to IPv4 format. */

if (insize == 4 && incoming[0] == 0 && incoming[1] == 0 &&
    incoming[2] == 0xffff)
  {
  insize = 1;
  incoming[0] = incoming[3];
  }

/* No match if the sizes don't agree. */

if (insize != size) return FALSE;

/* Else do the masked comparison. */

for (int i = 0; i < size; i++)
  {
  int mask;
  if (mlen == 0) mask = 0;
  else if (mlen < 32)
    {
    mask = (uint)(-1) << (32 - mlen);
    mlen = 0;
    }
  else
    {
    mask = -1;
    mlen -= 32;
    }
  if ((incoming[i] & mask) != (address[i] & mask)) return FALSE;
  }

return TRUE;
}



/*************************************************
*       Scan host list for local hosts           *
*************************************************/

/* Scan through a chain of addresses and check whether any of them is the
address of an interface on the local machine. If so, remove that address and
any previous ones with the same MX value, and all subsequent ones (which will
have greater or equal MX values) from the chain. Note: marking them as unusable
is NOT the right thing to do because it causes the hosts not to be used for
other domains, for which they may well be correct.

The hosts may be part of a longer chain; we only process those between the
initial pointer and the "last" pointer.

There is also a list of "pseudo-local" host names which are checked against the
host names. Any match causes that host item to be treated the same as one which
matches a local IP address.

If the very first host is a local host, then all MX records had a precedence
greater than or equal to that of the local host. Either there's a problem in
the DNS, or an apparently remote name turned out to be an abbreviation for the
local host. Give a specific return code, and let the caller decide what to do.
Otherwise, give a success code if at least one host address has been found.

Arguments:
  host        pointer to the first host in the chain
  lastptr     pointer to pointer to the last host in the chain (may be updated)
  removed     if not NULL, set TRUE if some local addresses were removed
                from the list

Returns:
  HOST_FOUND       if there is at least one host with an IP address on the chain
                     and an MX value less than any MX value associated with the
                     local host
  HOST_FOUND_LOCAL if a local host is among the lowest-numbered MX hosts; when
                     the host addresses were obtained from A records or
                     gethostbyname(), the MX values are set to -1.
  HOST_FIND_FAILED if no valid hosts with set IP addresses were found
*/

int
host_scan_for_local_hosts(host_item *host, host_item **lastptr, BOOL *removed)
{
int yield = HOST_FIND_FAILED;
host_item *last = *lastptr;
host_item *prev = NULL;
host_item *h;

if (removed) *removed = FALSE;

if (!local_interface_data) local_interface_data = host_find_interfaces();

for (h = host; h != last->next; h = h->next)
  {
#ifndef STAND_ALONE
  if (hosts_treat_as_local)
    {
    int rc;
    const uschar * save = deliver_domain;
    deliver_domain = h->name;   /* set $domain */
    rc = match_isinlist(string_copylc(h->name), CUSS &hosts_treat_as_local, 0,
      &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL);
    deliver_domain = save;
    if (rc == OK) goto FOUND_LOCAL;
    }
#endif

  /* It seems that on many operating systems, 0.0.0.0 is treated as a synonym
  for 127.0.0.1 and refers to the local host. We therefore force it always to
  be treated as local. */

  if (h->address)
    {
    if (Ustrcmp(h->address, "0.0.0.0") == 0) goto FOUND_LOCAL;
    for (ip_address_item * ip = local_interface_data; ip; ip = ip->next)
      if (Ustrcmp(h->address, ip->address) == 0) goto FOUND_LOCAL;
    yield = HOST_FOUND;  /* At least one remote address has been found */
    }

  /* Update prev to point to the last host item before any that have
  the same MX value as the one we have just considered. */

  if (!h->next || h->next->mx != h->mx)
    prev = h;
  }

return yield;  /* No local hosts found: return HOST_FOUND or HOST_FIND_FAILED */

/* A host whose IP address matches a local IP address, or whose name matches
something in hosts_treat_as_local has been found. */

FOUND_LOCAL:

if (!prev)
  {
  HDEBUG(D_host_lookup) debug_printf_indent(h->mx >= 0
    ? "local host has lowest MX\n"
    : "local host found for non-MX address\n");
  return HOST_FOUND_LOCAL;
  }

HDEBUG(D_host_lookup)
  {
  debug_printf_indent("local host in host list - removed hosts:\n");
  for (h = prev->next; h != last->next; h = h->next)
    debug_printf_indent("  %s %s %d\n", h->name, h->address, h->mx);
  }

if (removed) *removed = TRUE;
prev->next = last->next;
*lastptr = prev;
return yield;
}




/*************************************************
*        Remove duplicate IPs in host list       *
*************************************************/

/* You would think that administrators could set up their DNS records so that
one ended up with a list of unique IP addresses after looking up A or MX
records, but apparently duplication is common. So we scan such lists and
remove the later duplicates. Note that we may get lists in which some host
addresses are not set.

Arguments:
  host        pointer to the first host in the chain
  lastptr     pointer to pointer to the last host in the chain (may be updated)

Returns:      nothing
*/

static void
host_remove_duplicates(host_item *host, host_item **lastptr)
{
while (host != *lastptr)
  {
  if (host->address != NULL)
    {
    host_item *h = host;
    while (h != *lastptr)
      {
      if (h->next->address != NULL &&
          Ustrcmp(h->next->address, host->address) == 0)
        {
        DEBUG(D_host_lookup) debug_printf("duplicate IP address %s (MX=%d) "
          "removed\n", host->address, h->next->mx);
        if (h->next == *lastptr) *lastptr = h;
        h->next = h->next->next;
        }
      else h = h->next;
      }
    }
  /* If the last item was removed, host may have become == *lastptr */
  if (host != *lastptr) host = host->next;
  }
}




/*************************************************
*    Find sender host name by gethostbyaddr()    *
*************************************************/

/* This used to be the only way it was done, but it turns out that not all
systems give aliases for calls to gethostbyaddr() - or one of the modern
equivalents like getipnodebyaddr(). Fortunately, multiple PTR records are rare,
but they can still exist. This function is now used only when a DNS lookup of
the IP address fails, in order to give access to /etc/hosts.

Arguments:   none
Returns:     OK, DEFER, FAIL
*/

static int
host_name_lookup_byaddr(void)
{
struct hostent * hosts;
struct in_addr addr;
unsigned long time_msec = 0;	/* init to quieten dumb static analysis */

if (slow_lookup_log) time_msec = get_time_in_ms();

/* Lookup on IPv6 system */

#if HAVE_IPV6
if (Ustrchr(sender_host_address, ':') != NULL)
  {
  struct in6_addr addr6;
  if (inet_pton(AF_INET6, CS sender_host_address, &addr6) != 1)
    log_write_die(0, LOG_MAIN, "unable to parse %q as an "
      "IPv6 address", sender_host_address);
  #if HAVE_GETIPNODEBYADDR
  hosts = getipnodebyaddr(CS &addr6, sizeof(addr6), AF_INET6, &h_errno);
  #else
  hosts = gethostbyaddr(CS &addr6, sizeof(addr6), AF_INET6);
  #endif
  }
else
  {
  if (inet_pton(AF_INET, CS sender_host_address, &addr) != 1)
    log_write_die(0, LOG_MAIN, "unable to parse %q as an "
      "IPv4 address", sender_host_address);
  #if HAVE_GETIPNODEBYADDR
  hosts = getipnodebyaddr(CS &addr, sizeof(addr), AF_INET, &h_errno);
  #else
  hosts = gethostbyaddr(CS &addr, sizeof(addr), AF_INET);
  #endif
  }

/* Do lookup on IPv4 system */

#else
addr.s_addr = (S_ADDR_TYPE)inet_addr(CS sender_host_address);
hosts = gethostbyaddr(CS(&addr), sizeof(addr), AF_INET);
#endif

if (  slow_lookup_log
   && (time_msec = get_time_in_ms() - time_msec) > slow_lookup_log
   )
  log_long_lookup(US"gethostbyaddr", sender_host_address, time_msec);

/* Failed to look up the host. */

if (!hosts)
  {
  HDEBUG(D_host_lookup) debug_printf("IP address lookup failed: h_errno=%d\n",
    h_errno);
  return (h_errno == TRY_AGAIN || h_errno == NO_RECOVERY) ? DEFER : FAIL;
  }

/* It seems there are some records in the DNS that yield an empty name. We
treat this as non-existent. In some operating systems, this is returned as an
empty string; in others as a single dot. */

if (!hosts->h_name || !hosts->h_name[0] || hosts->h_name[0] == '.')
  {
  HDEBUG(D_host_lookup)
    debug_printf_indent("IP address lookup yielded an empty name: "
      "treated as non-existent host name\n");
  return FAIL;
  }

/* Copy and lowercase the name, which is in static storage in many systems.
Put it in permanent memory. */

  {
  int old_pool = store_pool;
  store_pool = POOL_TAINT_PERM;		/* names are tainted */

  sender_host_name = string_copylc(US hosts->h_name);

  /* If the host has aliases, build a copy of the alias list */

  if (hosts->h_aliases)
    {
    int count = 1;  /* need 1 more for terminating NULL */
    uschar **ptr;

    for (uschar ** aliases = USS hosts->h_aliases; *aliases; aliases++) count++;
    store_pool = POOL_PERM;
    ptr = sender_host_aliases = store_get(count * sizeof(uschar *), GET_UNTAINTED);
    store_pool = POOL_TAINT_PERM;

    for (uschar ** aliases = USS hosts->h_aliases; *aliases; aliases++)
      *ptr++ = string_copylc(*aliases);
    *ptr = NULL;
    }
  store_pool = old_pool;
  }

return OK;
}



/*************************************************
*        Find host name for incoming call        *
*************************************************/

/* Put the name in permanent store, pointed to by sender_host_name. We also set
up a list of alias names, pointed to by sender_host_alias. The list is
NULL-terminated. The incoming address is in sender_host_address, either in
dotted-quad form for IPv4 or in colon-separated form for IPv6.

This function does a thorough check that the names it finds point back to the
incoming IP address. Any that do not are discarded. Note that this is relied on
by the ACL reverse_host_lookup check.

On some systems, get{host,ipnode}byaddr() appears to do this internally, but
this it not universally true. Also, for release 4.30, this function was changed
to do a direct DNS lookup first, by default[1], because it turns out that that
is the only guaranteed way to find all the aliases on some systems. My
experiments indicate that Solaris gethostbyaddr() gives the aliases for but
Linux does not.

[1] The actual order is controlled by the host_lookup_order option.

Arguments:    none
Returns:      OK on success, the answer being placed in the global variable
                sender_host_name, with any aliases in a list hung off
                sender_host_aliases
              FAIL if no host name can be found
              DEFER if a temporary error was encountered

The variable host_lookup_msg is set to an empty string on success, or to a
reason for the failure otherwise, in a form suitable for tagging onto an error
message, and also host_lookup_failed is set TRUE if the lookup failed. If there
was a defer, host_lookup_deferred is set TRUE.

Any dynamically constructed string for host_lookup_msg must be in permanent
store, because it might be used for several incoming messages on the same SMTP
connection. */

int
host_name_lookup(void)
{
int sep = 0, old_pool, rc, yield;
uschar *save_hostname;
uschar **aliases;
uschar *ordername;
const uschar *list = host_lookup_order;
dns_answer * dnsa = store_get_dns_answer();
dns_scan dnss;

sender_host_dnssec = host_lookup_deferred = host_lookup_failed = FALSE;

HDEBUG(D_host_lookup)
  debug_printf_indent("looking up host name for %s\n", sender_host_address);
expand_level++;

/* For testing the case when a lookup does not complete, we have a special
reserved IP address. */

if (f.running_in_test_harness &&
    Ustrcmp(sender_host_address, "99.99.99.99") == 0)
  {
  HDEBUG(D_host_lookup)
    debug_printf("Test harness: host name lookup returns DEFER\n");
  host_lookup_deferred = TRUE;
  yield = DEFER;
  goto out;
  }

/* Do lookups directly in the DNS or via gethostbyaddr() (or equivalent), in
the order specified by the host_lookup_order option. */

while ((ordername = string_nextinlist(&list, &sep, NULL, 0)))
  {
  if (strcmpic(ordername, US"bydns") == 0)
    {
    uschar * name = dns_build_reverse(sender_host_address);

    dns_init(FALSE, FALSE, FALSE);    /* dnssec ctrl by dns_dnssec_ok glbl */
    rc = dns_lookup_timerwrap(dnsa, name, T_PTR, NULL);

    /* The first record we come across is used for the name; others are
    considered to be aliases. We have to scan twice, in order to find out the
    number of aliases. However, if all the names are empty, we will behave as
    if failure. (PTR records that yield empty names have been encountered in
    the DNS.) */

    if (rc == DNS_SUCCEED)
      {
      uschar **aptr = NULL;
      int ssize = 264;
      int count = 1;  /* need 1 more for terminating NULL */
      int old_pool = store_pool;

      sender_host_dnssec = dns_is_secure(dnsa);
      DEBUG(D_dns)
        debug_printf_indent("Reverse DNS security status: %s\n",
            sender_host_dnssec ? "DNSSEC verified (AD)" : "unverified");

      store_pool = POOL_PERM;        /* Save names in permanent storage */

      for (dns_record * rr = dns_next_rr(dnsa, &dnss, RESET_ANSWERS);
           rr;
           rr = dns_next_rr(dnsa, &dnss, RESET_NEXT)) if (rr->type == T_PTR)
	count++;

      /* Get store for the list of aliases. For compatibility with
      gethostbyaddr, we make an empty list if there are none. */

      aptr = sender_host_aliases = store_get(count * sizeof(uschar *), GET_UNTAINTED);

      /* Re-scan and extract the names */

      for (dns_record * rr = dns_next_rr(dnsa, &dnss, RESET_ANSWERS);
           rr;
           rr = dns_next_rr(dnsa, &dnss, RESET_NEXT)) if (rr->type == T_PTR)
        {
        uschar * s = store_get(ssize, GET_TAINTED);	/* names are tainted */
	unsigned slen;

        /* If an overlong response was received, the data will have been
        truncated and dn_expand may fail. */

        if (dn_expand(dnsa->answer, dnsa->answer + dnsa->answerlen,
             US rr->data, (DN_EXPAND_ARG4_TYPE)(s), ssize) < 0)
          {
          log_write(0, LOG_MAIN, "host name alias list truncated for %s",
            sender_host_address);
          break;
          }

        store_release_above(s + (slen = Ustrlen(s)) + 1);
        if (!*s)
          {
          HDEBUG(D_host_lookup) debug_printf_indent("IP address lookup yielded "
	    "an empty name: treated as non-existent host name\n");
          continue;
          }
	if (Ustrspn(s, letter_digit_hyphen_dot) != slen)
          {
          HDEBUG(D_host_lookup) debug_printf_indent("IP address lookup yielded "
	    "an illegal name (bad char): treated as non-existent host name\n");
          continue;
          }
        if (!sender_host_name) sender_host_name = s;
	else *aptr++ = s;
        while (*s) { *s = tolower(*s); s++; }
        }

      *aptr = NULL;            /* End of alias list */
      store_pool = old_pool;   /* Reset store pool */

      /* If we've found a name, break out of the "order" loop */

      if (sender_host_name) break;
      }

    /* If the DNS lookup deferred, we must also defer. */

    if (rc == DNS_AGAIN)
      {
      HDEBUG(D_host_lookup)
        debug_printf("IP address PTR lookup gave temporary error\n");
      host_lookup_deferred = TRUE;
      yield = DEFER;
      goto out;
      }
    }

  /* Do a lookup using gethostbyaddr() - or equivalent */

  else if (strcmpic(ordername, US"byaddr") == 0)
    {
    HDEBUG(D_host_lookup)
      debug_printf("IP address lookup using gethostbyaddr()\n");
    rc = host_name_lookup_byaddr();
    if (rc == DEFER)
      {
      host_lookup_deferred = TRUE;
      yield = rc;                       /* Can't carry on */
      goto out;
      }
    if (rc == OK) break;               /* Found a name */
    }
  }      /* Loop for bydns/byaddr scanning */

/* If we have failed to find a name, return FAIL and log when required.
NB host_lookup_msg must be in permanent store.  */

if (!sender_host_name)
  {
  if (host_checking || !f.log_testing_mode)
    log_write(L_host_lookup_failed, LOG_MAIN, "no host name found for IP "
      "address %s", sender_host_address);
  host_lookup_msg = US" (failed to find host name from IP address)";
  host_lookup_failed = TRUE;
  yield = FAIL;
  goto out;
  }

HDEBUG(D_host_lookup)
  {
  uschar ** aliases = sender_host_aliases;
  debug_printf_indent("IP address lookup yielded %q\n", sender_host_name);
  while (*aliases) debug_printf_indent("  alias %q\n", *aliases++);
  }

/* We need to verify that a forward lookup on the name we found does indeed
correspond to the address. This is for security: in principle a malefactor who
happened to own a reverse zone could set it to point to any names at all.

This code was present in versions of Exim before 3.20. At that point I took it
out because I thought that gethostbyaddr() did the check anyway. It turns out
that this isn't always the case, so it's coming back in at 4.01. This version
is actually better, because it also checks aliases.

The code was made more robust at release 4.21. Prior to that, it accepted all
the names if any of them had the correct IP address. Now the code checks all
the names, and accepts only those that have the correct IP address. */

save_hostname = sender_host_name;   /* Save for error messages */
aliases = sender_host_aliases;
for (uschar * hname = sender_host_name; hname; hname = *aliases++)
  {
  int rc;
  BOOL ok = FALSE;
  host_item h = { .next = NULL, .name = hname, .mx = MX_NONE, .address = NULL };
  dnssec_domains d =
    { .request = sender_host_dnssec ? US"*" : NULL, .require = NULL };

  if (  (rc = host_find_bydns(&h, NULL, HOST_FIND_BY_A | HOST_FIND_BY_AAAA,
	  NULL, NULL, NULL, &d, NULL, NULL)) == HOST_FOUND
     || rc == HOST_FOUND_LOCAL
     )
    {
    HDEBUG(D_host_lookup)
      debug_printf_indent("checking addresses for %s\n", hname);

    /* If the forward lookup was not secure we cancel the is-secure variable */

    DEBUG(D_dns) debug_printf_indent("Forward DNS security status: %s\n",
	  h.dnssec_used == DS_YES ? "DNSSEC verified (AD)" : "unverified");
    if (h.dnssec_used != DS_YES) sender_host_dnssec = FALSE;

    for (host_item * hh = &h; hh; hh = hh->next)
      if (host_is_in_net(hh->address, sender_host_address, 0))
        {
        HDEBUG(D_host_lookup) debug_printf("  %s OK\n", hh->address);
        ok = TRUE;
        break;
        }
      else
        HDEBUG(D_host_lookup) debug_printf("  %s\n", hh->address);

    if (!ok) HDEBUG(D_host_lookup)
      debug_printf("no IP address for %s matched %s\n", hname,
        sender_host_address);
    }
  else if (rc == HOST_FIND_AGAIN)
    {
    HDEBUG(D_host_lookup) debug_printf("temporary error for host name lookup\n");
    host_lookup_deferred = TRUE;
    sender_host_name = NULL;
    yield = DEFER;
    goto out;
    }
  else
    HDEBUG(D_host_lookup) debug_printf("no IP addresses found for %s\n", hname);

  /* If this name is no good, and it's the sender name, set it null pro tem;
  if it's an alias, just remove it from the list. */

  if (!ok)
    {
    if (hname == sender_host_name) sender_host_name = NULL; else
      {
      uschar **a;                              /* Don't amalgamate - some */
      a = --aliases;                           /* compilers grumble */
      while (*a != NULL) { *a = a[1]; a++; }
      }
    }
  }

/* If sender_host_name == NULL, it means we didn't like the name. Replace
it with the first alias, if there is one. */

if (!sender_host_name && *sender_host_aliases)
  sender_host_name = *sender_host_aliases++;

/* If we now have a main name, all is well. */

if (sender_host_name) { yield = OK; goto out; }

/* We have failed to find an address that matches. */

HDEBUG(D_host_lookup)
  debug_printf("%s does not match any IP address for %s\n",
    sender_host_address, save_hostname);

/* This message must be in permanent store */

old_pool = store_pool;
store_pool = POOL_PERM;
host_lookup_msg = string_sprintf(" (%s does not match any IP address for %s)",
  sender_host_address, save_hostname);
store_pool = old_pool;
host_lookup_failed = TRUE;
yield = FAIL;

out:
  expand_level--;
  return yield;
}




/*************************************************
*    Find IP address(es) for host by name        *
*************************************************/

/* The input is a host_item structure with the name filled in and the address
field set to NULL. We use gethostbyname() or getipnodebyname() or
gethostbyname2(), as appropriate. Of course, these functions may use the DNS,
but they do not do MX processing. It appears, however, that in some systems the
current setting of resolver options is used when one of these functions calls
the resolver. For this reason, we call dns_init() at the start, with arguments
influenced by bits in "flags", just as we do for host_find_bydns().

The second argument provides a host list (usually an IP list) of hosts to
ignore. This makes it possible to ignore IPv6 link-local addresses or loopback
addresses in unreasonable places.

The lookup may result in a change of name. For compatibility with the dns
lookup, return this via fully_qualified_name as well as updating the host item.
The lookup may also yield more than one IP address, in which case chain on
subsequent host_item structures.

Arguments:
  host                   a host item with the name and MX filled in;
                           the address is to be filled in;
                           multiple IP addresses cause other host items to be
                             chained on.
  ignore_target_hosts    a list of hosts to ignore
  flags                  HOST_FIND_QUALIFY_SINGLE   ) passed to
                         HOST_FIND_SEARCH_PARENTS   )   dns_init()
  fully_qualified_name   if not NULL, set to point to host name for
                         compatibility with host_find_bydns
  local_host_check       TRUE if a check for the local host is wanted

Returns:                 HOST_FIND_FAILED  Failed to find the host or domain
                         HOST_FIND_AGAIN   Try again later
                         HOST_FOUND        Host found - data filled in
                         HOST_FOUND_LOCAL  Host found and is the local host
*/

int
host_find_byname(host_item *host, const uschar *ignore_target_hosts, int flags,
  const uschar **fully_qualified_name, BOOL local_host_check)
{
int yield, times;
host_item *last = NULL;
BOOL temp_error = FALSE;
int af;

#ifndef DISABLE_TLS
/* Copy the host name at this point to the value which is used for
TLS certificate name checking, before anything modifies it.  */

host->certname = host->name;
#endif

/* Make sure DNS options are set as required. This appears to be necessary in
some circumstances when the get..byname() function actually calls the DNS. */

dns_init((flags & HOST_FIND_QUALIFY_SINGLE) != 0,
         (flags & HOST_FIND_SEARCH_PARENTS) != 0,
	 FALSE);		/* Cannot retrieve dnssec status so do not request */

/* In an IPv6 world, unless IPv6 has been disabled, we need to scan for both
kinds of address, so go round the loop twice. Note that we have ensured that
AF_INET6 is defined even in an IPv4 world, which makes for slightly tidier
code. However, if dns_ipv4_lookup matches the domain, we also just do IPv4
lookups here (except when testing standalone). */

#if HAVE_IPV6
  #ifdef STAND_ALONE
  if (disable_ipv6)
  #else
  if (  disable_ipv6
     ||    dns_ipv4_lookup
	&& match_isinlist(host->name, CUSS &dns_ipv4_lookup, 0,
	    &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL) == OK)
  #endif

    { af = AF_INET; times = 1; }
  else
    { af = AF_INET6; times = 2; }

/* No IPv6 support */

#else   /* HAVE_IPV6 */
  af = AF_INET; times = 1;
#endif  /* HAVE_IPV6 */

/* Initialize the flag that gets set for DNS syntax check errors, so that the
interface to this function can be similar to host_find_bydns. */

f.host_find_failed_syntax = FALSE;

/* Loop to look up both kinds of address in an IPv6 world */

for (int i = 1; i <= times;
     #if HAVE_IPV6
       af = AF_INET,     /* If 2 passes, IPv4 on the second */
     #endif
     i++)
  {
  BOOL ipv4_addr;
  int error_num = 0;
  struct hostent *hostdata;
  unsigned long time_msec = 0;	/* compiler quietening */

  #ifdef STAND_ALONE
  printf("Looking up: %s\n", host->name);
  #endif

  if (slow_lookup_log) time_msec = get_time_in_ms();

  #if HAVE_IPV6
  if (f.running_in_test_harness)
    hostdata = host_fake_gethostbyname(host->name, af, &error_num);
  else
    {
    #if HAVE_GETIPNODEBYNAME
    hostdata = getipnodebyname(CS host->name, af, 0, &error_num);
    #else
    hostdata = gethostbyname2(CS host->name, af);
    error_num = h_errno;
    #endif
    }

  #else    /* not HAVE_IPV6 */
  if (f.running_in_test_harness)
    hostdata = host_fake_gethostbyname(host->name, af, &error_num);
  else
    {
    hostdata = gethostbyname(CS host->name);
    error_num = h_errno;
    }
  #endif   /* HAVE_IPV6 */

  if (   slow_lookup_log
      && (time_msec = get_time_in_ms() - time_msec) > slow_lookup_log)
    log_long_lookup(US"gethostbyname", host->name, time_msec);

  if (!hostdata)
    {
    uschar * error;
    switch (error_num)
      {
      case HOST_NOT_FOUND: error = US"HOST_NOT_FOUND";	break;
      case TRY_AGAIN:      error = US"TRY_AGAIN";   temp_error = TRUE; break;
      case NO_RECOVERY:    error = US"NO_RECOVERY"; temp_error = TRUE; break;
      case NO_DATA:        error = US"NO_DATA";		break;
    #if NO_DATA != NO_ADDRESS
      case NO_ADDRESS:     error = US"NO_ADDRESS";	break;
    #endif
      default: error = US"?"; break;
      }

    DEBUG(D_host_lookup) debug_printf_indent("%s(af=%s) returned %d (%s)\n",
      f.running_in_test_harness ? "host_fake_gethostbyname" :
#if HAVE_IPV6
# if HAVE_GETIPNODEBYNAME
        "getipnodebyname",
# else
        "gethostbyname2",
# endif
#else
	"gethostbyname",
#endif
      af == AF_INET ? "inet" : "inet6", error_num, error);

    continue;
    }
  if (!(hostdata->h_addr_list)[0]) continue;

  /* Replace the name with the fully qualified one if necessary, and fill in
  the fully_qualified_name pointer. */

  if (hostdata->h_name[0] && Ustrcmp(host->name, hostdata->h_name) != 0)
    host->name = string_copy_dnsdomain(US hostdata->h_name);
  if (fully_qualified_name) *fully_qualified_name = host->name;

  /* Get the list of addresses. IPv4 and IPv6 addresses can be distinguished
  by their different lengths. Scan the list, ignoring any that are to be
  ignored, and build a chain from the rest. */

  ipv4_addr = hostdata->h_length == sizeof(struct in_addr);

  for (uschar ** addrlist = USS hostdata->h_addr_list; *addrlist; addrlist++)
    {
    uschar *text_address =
      host_ntoa(ipv4_addr? AF_INET:AF_INET6, *addrlist, NULL, NULL);

    #ifndef STAND_ALONE
    if (  ignore_target_hosts
       && verify_check_this_host(&ignore_target_hosts, NULL, host->name,
	    text_address, NULL) == OK)
      {
      DEBUG(D_host_lookup)
        debug_printf("ignored host %s [%s]\n", host->name, text_address);
      continue;
      }
    #endif

    /* If this is the first address, last is NULL and we put the data in the
    original block. */

    if (!last)
      {
      host->address = text_address;
      host->port = PORT_NONE;
      host->status = hstatus_unknown;
      host->why = hwhy_unknown;
      host->dnssec_used = DS_UNK;
#ifdef EXPERIMENTAL_SRV_SMTPS
      host->tls_needs = SRV_TLS_UNK;
#endif
      last = host;
      }

    /* Else add further host item blocks for any other addresses, keeping
    the order. */

    else
      {
      host_item *next = store_get(sizeof(host_item), GET_UNTAINTED);
      next->name = host->name;
#ifndef DISABLE_TLS
      next->certname = host->certname;
#endif
      next->mx = host->mx;
      next->address = text_address;
      next->port = PORT_NONE;
      next->status = hstatus_unknown;
      next->why = hwhy_unknown;
      next->dnssec_used = DS_UNK;
#ifdef EXPERIMENTAL_SRV_SMTPS
      next->tls_needs = SRV_TLS_UNK;
#endif
      next->last_try = 0;
      next->next = last->next;
      last->next = next;
      last = next;
      }
    }
  }

/* If no hosts were found, the address field in the original host block will be
NULL. If temp_error is set, at least one of the lookups gave a temporary error,
so we pass that back. */

if (!host->address)
  {
  uschar *msg =
#ifndef STAND_ALONE
    !message_id[0] && smtp_in_fd >= 0
      ? string_sprintf("no IP address found for host %s (during %s)", host->name,
          smtp_get_connection_info()) :
#endif
    string_sprintf("no IP address found for host %s", host->name);

  HDEBUG(D_host_lookup) debug_printf_indent("%s\n", msg);
  if (temp_error) goto RETURN_AGAIN;
  if (host_checking || !f.log_testing_mode)
    log_write(L_host_lookup_failed, LOG_MAIN, "%s", msg);
  return HOST_FIND_FAILED;
  }

/* Remove any duplicate IP addresses, then check to see if this is the local
host if required. */

host_remove_duplicates(host, &last);
yield = local_host_check?
  host_scan_for_local_hosts(host, &last, NULL) : HOST_FOUND;

HDEBUG(D_host_lookup)
  {
  if (fully_qualified_name)
    debug_printf("fully qualified name = %s\n", *fully_qualified_name);
  debug_printf("%s looked up these IP addresses:\n",
    #if HAVE_IPV6
      #if HAVE_GETIPNODEBYNAME
      "getipnodebyname"
      #else
      "gethostbyname2"
      #endif
    #else
    "gethostbyname"
    #endif
    );
  for (const host_item * h = host; h != last->next; h = h->next)
    debug_printf("  name=%s address=%s\n", h->name,
      h->address ? h->address : US"<null>");
  }

/* Return the found status. */

return yield;

/* Handle the case when there is a temporary error. If the name matches
dns_again_means_nonexist, return permanent rather than temporary failure. */

RETURN_AGAIN:
  {
#ifndef STAND_ALONE
  int rc;
  const uschar *save = deliver_domain;
  deliver_domain = host->name;  /* set $domain */
  rc = match_isinlist(host->name, CUSS &dns_again_means_nonexist, 0,
    &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL);
  deliver_domain = save;
  if (rc == OK)
    {
    DEBUG(D_host_lookup) debug_printf("%s is in dns_again_means_nonexist: "
      "returning HOST_FIND_FAILED\n", host->name);
    return HOST_FIND_FAILED;
    }
#endif
  return HOST_FIND_AGAIN;
  }
}



/*************************************************
*        Fill in a host address from the DNS     *
*************************************************/

/* Given a host item, with its name, port and mx fields set, and its address
field set to NULL, fill in its IP address from the DNS. If it is multi-homed,
create additional host items for the additional addresses, copying all the
other fields, and randomizing the order.

On IPv6 systems, AAAA records are sought first, then A records.

The host name may be changed if the DNS returns a different name - e.g. fully
qualified or changed via CNAME. If fully_qualified_name is not NULL, dns_lookup
ensures that it points to the fully qualified name. However, this is the fully
qualified version of the original name; if a CNAME is involved, the actual
canonical host name may be different again, and so we get it directly from the
relevant RR. Note that we do NOT change the mx field of the host item in this
function as it may be called to set the addresses of hosts taken from MX
records.

Arguments:
  host                  points to the host item we're filling in
  lastptr               points to pointer to last host item in a chain of
                          host items (may be updated if host is last and gets
                          extended because multihomed)
  ignore_target_hosts   list of hosts to ignore
  allow_ip              if TRUE, recognize an IP address and return it
  fully_qualified_name  if not NULL, return fully qualified name here if
                          the contents are different (i.e. it must be preset
                          to something)
  dnssec_request	if TRUE request the AD bit
  dnssec_require	if TRUE require the AD bit
  whichrrs		select ipv4, ipv6 results

Returns:       HOST_FIND_FAILED     couldn't find A record
               HOST_FIND_AGAIN      try again later
	       HOST_FIND_SECURITY   dnssec required but not acheived
               HOST_FOUND           found AAAA and/or A record(s)
               HOST_IGNORED         found, but all IPs ignored
*/

static int
set_address_from_dns(host_item *host, host_item **lastptr,
  const uschar *ignore_target_hosts, BOOL allow_ip,
  const uschar **fully_qualified_name,
  BOOL dnssec_request, BOOL dnssec_require, int whichrrs)
{
host_item * thishostlast = NULL;    /* Indicates not yet filled in anything */
BOOL v6_find_again = FALSE;
BOOL dnssec_fail = FALSE;
int i;
dns_answer * dnsa;

#ifndef DISABLE_TLS
/* Copy the host name at this point to the value which is used for
TLS certificate name checking, before any CNAME-following modifies it.  */

host->certname = host->name;
#endif

/* If allow_ip is set, a name which is an IP address returns that value
as its address. This is used for MX records when allow_mx_to_ip is set, for
those sites that feel they have to flaunt the RFC rules. */

if (allow_ip && string_is_ip_address(host->name, NULL) != 0)
  {
#ifndef STAND_ALONE
  if (  ignore_target_hosts
     && verify_check_this_host(&ignore_target_hosts, NULL, host->name,
        host->name, NULL) == OK)
    return HOST_IGNORED;
#endif

  host->address = host->name;
  return HOST_FOUND;
  }

dnsa = store_get_dns_answer();

/* On an IPv6 system, unless IPv6 is disabled, go round the loop up to twice,
looking for AAAA records the first time. However, unless doing standalone
testing, we force an IPv4 lookup if the domain matches dns_ipv4_lookup global.
On an IPv4 system, go round the loop once only, looking only for A records. */

#if HAVE_IPV6
# ifndef STAND_ALONE
    if (  disable_ipv6
       || !(whichrrs & HOST_FIND_BY_AAAA)
       ||    dns_ipv4_lookup
          && match_isinlist(host->name, CUSS &dns_ipv4_lookup, 0,
	      &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL) == OK
       )
      i = 0;    /* look up A records only */
    else
# endif        /* STAND_ALONE */

  i = 1;        /* look up AAAA and A records */

/* The IPv4 world */

#else           /* HAVE_IPV6 */
  i = 0;        /* look up A records only */
#endif          /* HAVE_IPV6 */

for (; i >= 0; i--)
  {
  static int types[] = { T_A, T_AAAA };
  int type = types[i];
  int randoffset = i == (whichrrs & HOST_FIND_IPV4_FIRST ? 1 : 0)
    ? 500 : 0;  /* Ensures v6/4 sort order */
  dns_scan dnss;

  int rc = dns_lookup_timerwrap(dnsa, host->name, type, fully_qualified_name);
  lookup_dnssec_authenticated = !dnssec_request ? NULL
    : dns_is_secure(dnsa) ? US"yes" : US"no";

  DEBUG(D_dns)
    if (  (dnssec_request || dnssec_require)
       && !dns_is_secure(dnsa)
       && dns_is_aa(dnsa)
       )
      debug_printf("DNS lookup of %.256s (A/AAAA) requested AD, but got AA\n", host->name);

  /* We want to return HOST_FIND_AGAIN if one of the A or AAAA lookups
  fails or times out, but not if another one succeeds. (In the early
  IPv6 days there are name servers that always fail on AAAA, but are happy
  to give out an A record. We want to proceed with that A record.) */

  if (rc != DNS_SUCCEED)
    {
    if (i == 0)  /* Just tried for an A record, i.e. end of loop */
      {
      if (host->address != NULL)
        i = HOST_FOUND;  /* AAAA was found */
      else if (rc == DNS_AGAIN || rc == DNS_FAIL || v6_find_again)
        i = HOST_FIND_AGAIN;
      else
	i = HOST_FIND_FAILED;    /* DNS_NOMATCH or DNS_NODATA */
      goto out;
      }

    /* Tried for an AAAA record: remember if this was a temporary
    error, and look for the next record type. */

    if (rc != DNS_NOMATCH && rc != DNS_NODATA) v6_find_again = TRUE;
    continue;
    }

  if (dnssec_request)
    {
    if (dns_is_secure(dnsa))
      {
      DEBUG(D_host_lookup) debug_printf("%s A DNSSEC\n", host->name);
      if (host->dnssec_used == DS_UNK) /* set in host_find_bydns() */
	host->dnssec_used = DS_YES;
      }
    else
      {
      if (dnssec_require)
	{
	dnssec_fail = TRUE;
	DEBUG(D_host_lookup) debug_printf("dnssec fail on %s for %.256s",
		i>0 ? "AAAA" : "A", host->name);
	continue;
	}
      if (host->dnssec_used == DS_YES) /* set in host_find_bydns() */
	{
	DEBUG(D_host_lookup) debug_printf("%s A cancel DNSSEC\n", host->name);
	host->dnssec_used = DS_NO;
	lookup_dnssec_authenticated = US"no";
	}
      }
    }

  /* Lookup succeeded: fill in the given host item with the first non-ignored
  address found; create additional items for any others. A single A6 record
  may generate more than one address.  The lookup had a chance to update the
  fqdn; we do not want any later times round the loop to do so. */

  fully_qualified_name = NULL;

  for (dns_record * rr = dns_next_rr(dnsa, &dnss, RESET_ANSWERS);
       rr;
       rr = dns_next_rr(dnsa, &dnss, RESET_NEXT)) if (rr->type == type)
    {
    dns_address * da = dns_address_from_rr(dnsa, rr);

    DEBUG(D_host_lookup)
      if (!da) debug_printf("no addresses extracted from A6 RR for %s\n",
	  host->name);

    /* This loop runs only once for A and AAAA records, but may run
    several times for an A6 record that generated multiple addresses. */

    for (; da; da = da->next)
      {
      #ifndef STAND_ALONE
      if (ignore_target_hosts != NULL &&
	    verify_check_this_host(&ignore_target_hosts, NULL,
	      host->name, da->address, NULL) == OK)
	{
	DEBUG(D_host_lookup)
	  debug_printf("ignored host %s [%s]\n", host->name, da->address);
	continue;
	}
      #endif

      /* If this is the first address, stick it in the given host block,
      and change the name if the returned RR has a different name. */

      if (thishostlast == NULL)
	{
	if (strcmpic(host->name, rr->name) != 0)
	  host->name = string_copy_dnsdomain(rr->name);
	host->address = da->address;
	host->sort_key = host->mx * 1000 + random_number(500) + randoffset;
	host->status = hstatus_unknown;
	host->why = hwhy_unknown;
	thishostlast = host;
	}

      /* Not the first address. Check for, and ignore, duplicates. Then
      insert in the chain at a random point. */

      else
	{
	int new_sort_key;
	host_item *next;

	/* End of our local chain is specified by "thishostlast". */

	for (next = host;; next = next->next)
	  {
	  if (Ustrcmp(CS da->address, next->address) == 0) break;
	  if (next == thishostlast) { next = NULL; break; }
	  }
	if (next != NULL) continue;  /* With loop for next address */

	/* Not a duplicate */

	new_sort_key = host->mx * 1000 + random_number(500) + randoffset;
	next = store_get(sizeof(host_item), GET_UNTAINTED);

	/* New address goes first: insert the new block after the first one
	(so as not to disturb the original pointer) but put the new address
	in the original block. */

	if (new_sort_key < host->sort_key)
	  {
	  *next = *host;                          /* Copies port & tls_needs */
	  host->next = next;
	  host->address = da->address;
	  host->sort_key = new_sort_key;
	  if (thishostlast == host) thishostlast = next;  /* Local last */
	  if (*lastptr == host) *lastptr = next;          /* Global last */
	  }

	/* Otherwise scan down the addresses for this host to find the
	one to insert after. */

	else
	  {
	  host_item *h = host;
	  while (h != thishostlast)
	    {
	    if (new_sort_key < h->next->sort_key) break;
	    h = h->next;
	    }
	  *next = *h;                                 /* Copies port */
	  h->next = next;
	  next->address = da->address;
	  next->sort_key = new_sort_key;
	  if (h == thishostlast) thishostlast = next; /* Local last */
	  if (h == *lastptr) *lastptr = next;         /* Global last */
	  }
	}
      }
    }
  }

/* Control gets here only if the second lookup (the A record) succeeded.
However, the address may not be filled in if it was ignored. */

i = host->address
  ? HOST_FOUND
  : dnssec_fail
  ? HOST_FIND_SECURITY
  : HOST_IGNORED;

out:
  store_free_dns_answer(dnsa);
  return i;
}




/*************************************************
*    Find IP addresses and host names via DNS    *
*************************************************/

/* The input is a host_item structure with the name field filled in and the
address field set to NULL. This may be in a chain of other host items. The
lookup may result in more than one IP address, in which case we must created
new host blocks for the additional addresses, and insert them into the chain.
The original name may not be fully qualified. Use the fully_qualified_name
argument to return the official name, as returned by the resolver.

Arguments:
  host                  point to initial host item
  ignore_target_hosts   a list of hosts to ignore
  whichrrs              flags indicating which RRs to look for:
                          HOST_FIND_BY_SRV  => look for SRV
                          HOST_FIND_BY_MX   => look for MX
                          HOST_FIND_BY_A    => look for A
                          HOST_FIND_BY_AAAA => look for AAAA
                        also flags indicating how the lookup is done
                          HOST_FIND_QUALIFY_SINGLE   ) passed to the
                          HOST_FIND_SEARCH_PARENTS   )   resolver
			  HOST_FIND_IPV4_FIRST => reverse usual result ordering
			  HOST_FIND_IPV4_ONLY  => MX results elide ipv6
  srv_svclist		when SRV used, the service name list
  srv_fail_domains      DNS errors for these domains => assume nonexist
  mx_fail_domains       DNS errors for these domains => assume nonexist
  dnssec_d.request =>   make dnssec request: domainlist
  dnssec_d.require =>   ditto and nonexist failures
  fully_qualified_name  if not NULL, return fully-qualified name
  removed               set TRUE if local host was removed from the list

Returns:                HOST_FIND_FAILED  Failed to find the host or domain;
                                          if there was a syntax error,
                                          host_find_failed_syntax is set.
                        HOST_FIND_AGAIN   Could not resolve at this time
			HOST_FIND_SECURITY dnsssec required but not acheived
                        HOST_FOUND        Host found
                        HOST_FOUND_LOCAL  The lowest MX record points to this
                                          machine, if MX records were found, or
                                          an A record that was found contains
                                          an address of the local host
*/

int
host_find_bydns(host_item * host, const uschar * ignore_target_hosts,
  int whichrrs,
  const uschar * srv_svclist, const uschar * srv_fail_domains,
  const uschar * mx_fail_domains, const dnssec_domains * dnssec_d,
  const uschar ** fully_qualified_name, BOOL * removed)
{
host_item * h, * last;
#ifdef EXPERIMENTAL_SRV_SMTPS
BOOL srv_smtps = FALSE;
#endif
int rc = DNS_FAIL, ind_type = 0, yield;
dns_answer * dnsa = store_get_dns_answer();
dns_scan dnss;
BOOL dnssec_require, dnssec_request;
dnssec_status_t dnssec;

HDEBUG(D_host_lookup)
  {
  debug_printf_indent("check dnssec require list\n");
  expand_level++;
  }
dnssec_require = dnssec_d
  && match_isinlist(host->name, CUSS &dnssec_d->require,
		  0, &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL) == OK;

HDEBUG(D_host_lookup)
  {
  expand_level--;
  debug_printf_indent("check dnssec request list\n");
  expand_level++;
  }
dnssec_request = dnssec_require
    || (  dnssec_d
       && match_isinlist(host->name, CUSS &dnssec_d->request,
		    0, &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL) == OK);
HDEBUG(D_host_lookup)
  expand_level--;

/* Set the default fully qualified name to the incoming name, initialize the
resolver if necessary, set up the relevant options, and initialize the flag
that gets set for DNS syntax check errors. */

if (fully_qualified_name) *fully_qualified_name = host->name;
dns_init((whichrrs & HOST_FIND_QUALIFY_SINGLE) != 0,
         (whichrrs & HOST_FIND_SEARCH_PARENTS) != 0,
	 dnssec_request);
f.host_find_failed_syntax = FALSE;

/* First, if requested, look for SRV records. The service name is given; we
assume TCP protocol. DNS domain names are constrained to a maximum of 256
characters, so the code below should be safe. */

if (whichrrs & HOST_FIND_BY_SRV)
  {
  int sep = 0;
  for (const uschar * srv_service;
       srv_service = string_nextinlist(&srv_svclist, &sep, NULL, 0); )
    {
    uschar * s, * temp_fully_qualified_name;
    int prefix_length;

    s = string_sprintf("_%s._tcp.%n%.256s",
	  srv_service, &prefix_length, host->name);
    temp_fully_qualified_name = s;
    ind_type = T_SRV;

    /* Search for SRV records. If the fully qualified name is different to
    the input name, pass back the new original domain, without the prepended
    magic. */

    dnssec = DS_UNK;
    lookup_dnssec_authenticated = NULL;
    rc = dns_lookup_timerwrap(dnsa, temp_fully_qualified_name, ind_type,
	  CUSS &temp_fully_qualified_name);

    DEBUG(D_dns)
      if ((dnssec_request || dnssec_require)
	  && !dns_is_secure(dnsa)
	  && dns_is_aa(dnsa))
	debug_printf_indent("DNS lookup of %.256s (SRV) requested AD, but got AA\n", host->name);

    if (dnssec_request)
      {
      if (dns_is_secure(dnsa))
	{ dnssec = DS_YES; lookup_dnssec_authenticated = US"yes"; }
      else
	{ dnssec = DS_NO; lookup_dnssec_authenticated = US"no"; }
      }

    if (temp_fully_qualified_name != s && fully_qualified_name)
      *fully_qualified_name = temp_fully_qualified_name + prefix_length;
      
    /* On DNS failures, we give the "try again" error unless the domain is
    listed as one for which we continue. */

    if (rc == DNS_SUCCEED && dnssec_require && !dns_is_secure(dnsa))
      {
      log_write(L_host_lookup_failed, LOG_MAIN,
		  "dnssec fail on SRV for %.256s", host->name);
      rc = DNS_FAIL;
      }
    if (rc == DNS_FAIL || rc == DNS_AGAIN)
      {
#ifndef STAND_ALONE
      if (match_isinlist(host->name, &srv_fail_domains, 0,
	  &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL) != OK)
#endif
	{ yield = HOST_FIND_AGAIN; goto out; }
      DEBUG(D_host_lookup) debug_printf_indent("DNS_%s treated as DNS_NODATA "
	"(domain in srv_fail_domains)\n", rc == DNS_FAIL ? "FAIL":"AGAIN");
      }
    else if (rc == DNS_SUCCEED)
      {
#ifdef EXPERIMENTAL_SRV_SMTPS
      if (Ustrcmp(srv_service, SRV_SMTPS_SERVICE_NAME) == 0)
	srv_smtps = TRUE;			/* force tls-on-connect */
#endif
      break;				/* walking the service names list */
      }
    }
  }

/* If we did not find any SRV records, search the DNS for MX records, if
requested to do so. If the result is DNS_NOMATCH, it means there is no such
domain, and there's no point in going on to look for address records with the
same domain. The result will be DNS_NODATA if the domain exists but has no MX
records. On DNS failures, we give the "try again" error unless the domain is
listed as one for which we continue. */

if (rc != DNS_SUCCEED  &&  whichrrs & HOST_FIND_BY_MX)
  {
  ind_type = T_MX;
  dnssec = DS_UNK;
  lookup_dnssec_authenticated = NULL;
  rc = dns_lookup_timerwrap(dnsa, host->name, ind_type, fully_qualified_name);

  DEBUG(D_dns)
    if (  (dnssec_request || dnssec_require)
       && !dns_is_secure(dnsa)
       && dns_is_aa(dnsa))
      debug_printf_indent("DNS lookup of %.256s (MX) requested AD, but got AA\n", host->name);

  if (dnssec_request)
    if (dns_is_secure(dnsa))
      {
      DEBUG(D_host_lookup)
	debug_printf_indent("%s (MX resp) DNSSEC\n", host->name);
      dnssec = DS_YES; lookup_dnssec_authenticated = US"yes";
      }
    else
      {
      dnssec = DS_NO; lookup_dnssec_authenticated = US"no";
      }

  switch (rc)
    {
    case DNS_NOMATCH:
      yield = HOST_FIND_FAILED;
      goto out;

    case DNS_SUCCEED:
      if (!dnssec_require || dns_is_secure(dnsa))
	break;
      DEBUG(D_host_lookup)
	debug_printf_indent("dnssec fail on MX for %.256s\n", host->name);
#ifndef STAND_ALONE
      if (match_isinlist(host->name, CUSS &mx_fail_domains, 0,
	  &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL) != OK)
	{ yield = HOST_FIND_SECURITY; goto out; }
#endif
      rc = DNS_FAIL;
      /*FALLTHROUGH*/

    case DNS_FAIL:
    case DNS_AGAIN:
#ifndef STAND_ALONE
      if (match_isinlist(host->name, CUSS &mx_fail_domains, 0,
	  &domainlist_anchor, NULL, MCL_DOMAIN, TRUE, NULL) != OK)
#endif
	{ yield = HOST_FIND_AGAIN; goto out; }
      DEBUG(D_host_lookup) debug_printf_indent("DNS_%s treated as DNS_NODATA "
	"(domain in mx_fail_domains)\n", (rc == DNS_FAIL)? "FAIL":"AGAIN");
      break;
    }
  }

/* If we haven't found anything yet, and we are requested to do so, try for an
A or AAAA record. If we find it (or them) check to see that it isn't the local
host. */

if (rc != DNS_SUCCEED)
  {
  if (!(whichrrs & (HOST_FIND_BY_A | HOST_FIND_BY_AAAA)))
    {
    DEBUG(D_host_lookup) debug_printf_indent("Address records are not being sought\n");
    yield = HOST_FIND_FAILED;
    goto out;
    }

  last = host;        /* End of local chainlet */
  host->mx = MX_NONE;
  host->port = PORT_NONE;
  host->dnssec_used = DS_UNK;
#ifdef EXPERIMENTAL_SRV_SMTPS
  host->tls_needs = SRV_TLS_UNK;
#endif
  lookup_dnssec_authenticated = NULL;
  rc = set_address_from_dns(host, &last, ignore_target_hosts, FALSE,
    fully_qualified_name, dnssec_request, dnssec_require, whichrrs);

  /* If one or more address records have been found, check that none of them
  are local. Since we know the host items all have their IP addresses
  inserted, host_scan_for_local_hosts() can only return HOST_FOUND or
  HOST_FOUND_LOCAL. We do not need to scan for duplicate IP addresses here,
  because set_address_from_dns() removes them. */

  if (rc == HOST_FOUND)
    rc = host_scan_for_local_hosts(host, &last, removed);
  else if (rc == HOST_IGNORED)
    rc = HOST_FIND_FAILED;  			/* No special action */

  DEBUG(D_host_lookup)
    if (host->address)
      {
      if (fully_qualified_name)
        debug_printf_indent("fully qualified name = %s\n", *fully_qualified_name);
      for (host_item * h = host; h != last->next; h = h->next)
        debug_printf_indent("%s %s mx=%d sort=%d %s\n", h->name,
          h->address ? h->address : US"<null>", h->mx, h->sort_key,
          h->status >= hstatus_unusable ? US"*" : US"");
      }

  yield = rc;
  goto out;
  }

/* We have found one or more SRV or MX records. Sort them according to
precedence. Put the data for the first one into the existing host block, and
insert new host_item blocks into the chain for the remainder. For equal
precedences one is supposed to randomize the order. To make this happen, the
sorting is actually done on the MX value * 1000 + a random number. This is put
into a host field called sort_key.

In the case of hosts with both IPv6 and IPv4 addresses, we want to choose the
IPv6 address in preference. At this stage, we don't know what kind of address
the host has. We choose a random number < 500; if later we find an A record
first, we add 500 to the random number. Then for any other address records, we
use random numbers in the range 0-499 for AAAA records and 500-999 for A
records.

At this point we remove any duplicates that point to the same host, retaining
only the one with the lowest precedence. We cannot yet check for precedence
greater than that of the local host, because that test cannot be properly done
until the addresses have been found - an MX record may point to a name for this
host which is not the primary hostname. */

last = NULL;    /* Indicates that not even the first item is filled yet */

for (dns_record * rr = dns_next_rr(dnsa, &dnss, RESET_ANSWERS);
     rr;
     rr = dns_next_rr(dnsa, &dnss, RESET_NEXT)) if (rr->type == ind_type)
  {
  int precedence, weight, sort_key;
  int port = PORT_NONE;		/* MX lookups get PORT_NONE */
  const uschar * s = rr->data;	/* MUST be unsigned for GETSHORT */
  host_item * next;
  uschar data[256];

  if (rr_bad_size(rr, sizeof(uint16_t))) continue;
  GETSHORT(precedence, s);      /* Pointer s is advanced */

  /* For MX records, we use a random "weight" which causes multiple records of
  the same precedence to sort randomly. */

  if (ind_type == T_MX)
    weight = random_number(500);
  else
    {
    /* SRV records are specified with a port and a weight. The weight is used
    in a special algorithm. However, to start with, we just use it to order the
    records of equal priority (precedence). */

    if (rr_bad_increment(rr, s, 2 * sizeof(uint16_t))) continue;
    GETSHORT(weight, s);
    GETSHORT(port, s);
#ifdef EXPERIMENTAL_SRV_SMTPS
    if (port == 25 && srv_smtps) port = PORT_NONE;	/* no-fallback STARTTLS */
#endif
    }
  sort_key = precedence * 1000 + weight;

  /* Get the name of the host pointed to. */

  (void)dn_expand(dnsa->answer, dnsa->answer + dnsa->answerlen, s,
    (DN_EXPAND_ARG4_TYPE)data, sizeof(data));

  /* Check that we haven't already got this host on the chain; if we have,
  keep only the lower precedence. This situation shouldn't occur, but you
  never know what junk might get into the DNS (and this case has been seen on
  more than one occasion). */

  if (last)       /* This is not the first record */
    {
    host_item * prev = NULL;

    for (h = host; h != last->next; prev = h, h = h->next)
      if (strcmpic(h->name, data) == 0)
        {
        DEBUG(D_host_lookup)
          debug_printf_indent("discarded duplicate host %s (MX=%d)\n", data,
            precedence > h->mx ? precedence : h->mx);
        if (precedence >= h->mx) goto NEXT_MX_RR; /* Skip greater precedence */
        if (h == host)                            /* Override first item */
          {
          h->mx = precedence;
          host->sort_key = sort_key;
          goto NEXT_MX_RR;
          }

        /* Unwanted host item is not the first in the chain, so we can get
        get rid of it by cutting it out. */

        prev->next = h->next;
        if (h == last) last = prev;
        break;
        }
    }

  /* If this is the first MX or SRV record, put the data into the existing host
  block. Otherwise, add a new block in the correct place; if it has to be
  before the first block, copy the first block's data to a new second block. */

  next = last ? store_get(sizeof(host_item), GET_UNTAINTED) : host;
  next->name = string_copy_dnsdomain(data);
  next->address = NULL;
  next->port = port;
  next->mx = precedence;
  next->status = hstatus_unknown;
  next->why = hwhy_unknown;
  next->dnssec_used = dnssec;
#ifdef EXPERIMENTAL_SRV_SMTPS
  next->tls_needs = srv_smtps
    ? port == PORT_NONE ? SRV_STARTTLS_MUST : SRV_TLS_ON_CONNECT
    : SRV_TLS_UNK;
#endif
  next->sort_key = sort_key;

  if (!last)		/* we're using the first, supplied, host block */
    last = host;
  else			/* a newly allocated block */
    {
    next->last_try = 0;

    /* Handle the case when we have to insert before the first item. */

    if (sort_key < host->sort_key)
      {
      host_item htemp = *host;
      *host = *next;
      *next = htemp;
      host->next = next;
      if (last == host) last = next;
      }
    else /* Scan down the items we have inserted as part of this exercise;
	 don't go further. */
      {
      for (h = host; h != last; h = h->next)
        if (sort_key < h->next->sort_key)
          {
          next->next = h->next;
          h->next = next;
          break;
          }

      /* Join on after the last host item that's part of this
      processing if we haven't stopped sooner. */

      if (h == last)
        {
        next->next = last->next;
        last->next = next;
        last = next;
        }
      }
    }

  NEXT_MX_RR: continue;
  }

if (!last)	/* No rr of correct type; give up */
  {
  yield = HOST_FIND_FAILED;
  goto out;
  }

/* If the list of hosts was obtained from SRV records, there are two things to
do. First, if there is only one host, and it's name is ".", it means there is
no SMTP service at this domain. Otherwise, we have to sort the hosts of equal
priority according to their weights, using an algorithm that is defined in RFC
2782. The hosts are currently sorted by priority and weight. For each priority
group we have to pick off one host and put it first, and then repeat for any
remaining in the same priority group. */

if (ind_type == T_SRV)
  {
  host_item ** pptr;

  if (host == last && host->name[0] == 0)
    {
    DEBUG(D_host_lookup) debug_printf_indent("the single SRV record is \".\"\n");
    yield = HOST_FIND_FAILED;
    goto out;
    }

  DEBUG(D_host_lookup)
    {
    debug_printf_indent("original ordering of hosts from SRV records:\n");
    for (h = host; h != last->next; h = h->next)
      debug_printf_indent("  %s P=%d W=%d\n", h->name, h->mx, h->sort_key % 1000);
    }

  for (pptr = &host, h = host; h != last; pptr = &h->next, h = h->next)
    {
    int sum = 0;
    host_item *hh;

    /* Find the last following host that has the same precedence. At the same
    time, compute the sum of the weights and the running totals. These can be
    stored in the sort_key field. */

    for (hh = h; hh != last; hh = hh->next)
      {
      int weight = hh->sort_key % 1000;   /* was precedence * 1000 + weight */
      sum += weight;
      hh->sort_key = sum;
      if (hh->mx != hh->next->mx) break;
      }

    /* If there's more than one host at this precedence (priority), we need to
    pick one to go first. */

    if (hh != h)
      {
      host_item *hhh;
      host_item **ppptr;
      int randomizer = random_number(sum + 1);

      for (ppptr = pptr, hhh = h;
           hhh != hh;
           ppptr = &hhh->next, hhh = hhh->next)
        if (hhh->sort_key >= randomizer)
	  break;

      /* hhh now points to the host that should go first; ppptr points to the
      place that points to it. Unfortunately, if the start of the minilist is
      the start of the entire list, we can't just swap the items over, because
      we must not change the value of host, since it is passed in from outside.
      One day, this could perhaps be changed.

      The special case is fudged by putting the new item *second* in the chain,
      and then transferring the data between the first and second items. We
      can't just swap the first and the chosen item, because that would mean
      that an item with zero weight might no longer be first. */

      if (hhh != h)
        {
        *ppptr = hhh->next;          /* Cuts it out of the chain */

        if (h == host)
          {
          host_item temp = *h;
          *h = *hhh;
          *hhh = temp;
          hhh->next = temp.next;
          h->next = hhh;
          }
        else
          {
          hhh->next = h;               /* The rest of the chain follows it */
          *pptr = hhh;                 /* It takes the place of h */
          h = hhh;                     /* It's now the start of this minilist */
          }
        }
      }

    /* A host has been chosen to be first at this priority and h now points
    to this host. There may be others at the same priority, or others at a
    different priority. Before we leave this host, we need to put back a sort
    key of the traditional MX kind, in case this host is multihomed, because
    the sort key is used for ordering the multiple IP addresses. We do not need
    to ensure that these new sort keys actually reflect the order of the hosts,
    however. */

    h->sort_key = h->mx * 1000 + random_number(500);
    }   /* Move on to the next host */
  }

/* Now we have to find IP addresses for all the hosts. We have ensured above
that the names in all the host items are unique. Before release 4.61 we used to
process records from the additional section in the DNS packet that returned the
MX or SRV records. However, a DNS name server is free to drop any resource
records from the additional section. In theory, this has always been a
potential problem, but it is exacerbated by the advent of IPv6. If a host had
several IPv4 addresses and some were not in the additional section, at least
Exim would try the others. However, if a host had both IPv4 and IPv6 addresses
and all the IPv4 (say) addresses were absent, Exim would try only for a IPv6
connection, and never try an IPv4 address. When there was only IPv4
connectivity, this was a disaster that did in practice occur.

So, from release 4.61 onwards, we always search for A and AAAA records
explicitly. The names shouldn't point to CNAMES, but we use the general lookup
function that handles them, just in case. If any lookup gives a soft error,
change the default yield.

For these DNS lookups, we must disable qualify_single and search_parents;
otherwise invalid host names obtained from MX or SRV records can cause trouble
if they happen to match something local. */

yield = HOST_FIND_FAILED;    /* Default yield */
dns_init(FALSE, FALSE,       /* Disable qualify_single and search_parents */
	 dnssec_request || dnssec_require);

for (h = host; h != last->next; h = h->next)
  {
  if (h->address) continue;  /* Inserted by a multihomed host */

  rc = set_address_from_dns(h, &last, ignore_target_hosts, allow_mx_to_ip,
    NULL, dnssec_request, dnssec_require,
    whichrrs & HOST_FIND_IPV4_ONLY
    ?  HOST_FIND_BY_A  :  HOST_FIND_BY_A | HOST_FIND_BY_AAAA);
  if (rc != HOST_FOUND)
    {
    h->status = hstatus_unusable;
    switch (rc)
      {
      case HOST_FIND_AGAIN:	yield = rc; h->why = hwhy_deferred; break;
      case HOST_FIND_SECURITY:	yield = rc; h->why = hwhy_insecure; break;
      case HOST_IGNORED:	h->why = hwhy_ignored; break;
      default:			h->why = hwhy_failed; break;
      }
    }
  }

/* Scan the list for any hosts that are marked unusable because they have
been explicitly ignored, and remove them from the list, as if they did not
exist. If we end up with just a single, ignored host, flatten its fields as if
nothing was found. */

if (ignore_target_hosts)
  {
  host_item *prev = NULL;
  for (h = host; h != last->next; h = h->next)
    {
    REDO:
    if (h->why != hwhy_ignored)        /* Non ignored host, just continue */
      prev = h;
    else if (prev == NULL)             /* First host is ignored */
      {
      if (h != last)                   /* First is not last */
        {
        if (h->next == last) last = h; /* Overwrite it with next */
        *h = *(h->next);               /* and reprocess it. */
        goto REDO;                     /* C should have redo, like Perl */
        }
      }
    else                               /* Ignored host is not first - */
      {                                /*   cut it out */
      prev->next = h->next;
      if (h == last) last = prev;
      }
    }

  if (host->why == hwhy_ignored) host->address = NULL;
  }

/* There is still one complication in the case of IPv6. Although the code above
arranges that IPv6 addresses take precedence over IPv4 addresses for multihomed
hosts, it doesn't do this for addresses that apply to different hosts with the
same MX precedence, because the sorting on MX precedence happens first. So we
have to make another pass to check for this case. We ensure that, within a
single MX preference value, IPv6 addresses come first. This can separate the
addresses of a multihomed host, but that should not matter. */

#if HAVE_IPV6
if (h != last && !disable_ipv6) for (h = host; h != last; h = h->next)
  {
  host_item temp;
  host_item *next = h->next;

  if (  h->mx != next->mx			/* If next is different MX */
     || !h->address				/* OR this one is unset */
     )
    continue;					/* move on to next */

  if (  whichrrs & HOST_FIND_IPV4_FIRST
     ?     !Ustrchr(h->address, ':')		/* OR this one is IPv4 */
        || next->address
           && Ustrchr(next->address, ':')	/* OR next is IPv6 */

     :     Ustrchr(h->address, ':')		/* OR this one is IPv6 */
        || next->address
           && !Ustrchr(next->address, ':')	/* OR next is IPv4 */
     )
    continue;                                /* move on to next */

  temp = *h;                                 /* otherwise, swap */
  temp.next = next->next;
  *h = *next;
  h->next = next;
  *next = temp;
  }
#endif	/*HAVE_IPV6*/

/* Remove any duplicate IP addresses and then scan the list of hosts for any
whose IP addresses are on the local host. If any are found, all hosts with the
same or higher MX values are removed. However, if the local host has the lowest
numbered MX, then HOST_FOUND_LOCAL is returned. Otherwise, if at least one host
with an IP address is on the list, HOST_FOUND is returned. Otherwise,
HOST_FIND_FAILED is returned, but in this case do not update the yield, as it
might have been set to HOST_FIND_AGAIN just above here. If not, it will already
be HOST_FIND_FAILED. */

host_remove_duplicates(host, &last);
rc = host_scan_for_local_hosts(host, &last, removed);
if (rc != HOST_FIND_FAILED) yield = rc;

DEBUG(D_host_lookup)
  {
  if (fully_qualified_name)
    debug_printf_indent("fully qualified name = %s\n", *fully_qualified_name);
  debug_printf_indent("host_find_bydns yield = %s (%d); returned hosts:\n",
    yield == HOST_FOUND		? "HOST_FOUND" :
    yield == HOST_FOUND_LOCAL	? "HOST_FOUND_LOCAL" :
    yield == HOST_FIND_SECURITY	? "HOST_FIND_SECURITY" :
    yield == HOST_FIND_AGAIN	? "HOST_FIND_AGAIN" :
    yield == HOST_FIND_FAILED	? "HOST_FIND_FAILED" : "?",
    yield);
  for (h = host; h != last->next; h = h->next)
    {
    debug_printf_indent("  %s %s MX=%d %s", h->name,
      !h->address ? US"<null>" : h->address, h->mx,
      h->dnssec_used == DS_YES ? US"DNSSEC " : US"");
    if (h->port != PORT_NONE) debug_printf("port=%d ", h->port);
    if (h->status >= hstatus_unusable) debug_printf("*");
    debug_printf("\n");
    }
  }

out:

dns_init(FALSE, FALSE, FALSE);	/* clear the dnssec bit for getaddrbyname */
store_free_dns_answer(dnsa);
return yield;
}




#ifdef SUPPORT_DANE
/* Lookup TLSA record for host/port.
Return:  OK		success with dnssec; DANE mode
         DEFER		Do not use this host now, may retry later
	 FAIL_FORCED	No TLSA record; DANE not usable
	 FAIL		Do not use this connection
*/

int
tlsa_lookup(const host_item * host, dns_answer * dnsa, BOOL dane_required)
{
uschar buffer[300];
const uschar * fullname = buffer;
int rc;
BOOL sec;

/* TLSA lookup string */
(void)sprintf(CS buffer, "_%d._tcp.%.256s", host->port, host->name);

rc = dns_lookup_timerwrap(dnsa, buffer, T_TLSA, &fullname);
sec = dns_is_secure(dnsa);
DEBUG(D_transport)
  debug_printf("TLSA lookup ret %s %sDNSSEC\n", dns_rc_names[rc], sec ? "" : "not ");

switch (rc)
  {
  case DNS_AGAIN:
    return DEFER; /* just defer this TLS'd conn */

  case DNS_SUCCEED:
    if (sec)
      {
      DEBUG(D_transport)
	{
	dns_scan dnss;
	for (dns_record * rr = dns_next_rr(dnsa, &dnss, RESET_ANSWERS); rr;
	     rr = dns_next_rr(dnsa, &dnss, RESET_NEXT))
	  if (rr->type == T_TLSA && rr->size > 3)
	    {
	    int payload_length = rr->size - 3;
	    uschar * p = US rr->data;
	    uint8_t usage = *p++, selector = *p++, matching_type = *p++;

	    if (payload_length > MAX_TLSA_EXPANDED_SIZE)
	      payload_length = MAX_TLSA_EXPANDED_SIZE;
	    debug_printf(" %d %d %d %.*H\n",
			  usage, selector, matching_type, payload_length, p);
	    }
	}
      return OK;
      }
    log_write(0, LOG_MAIN,
      "DANE error: TLSA lookup for %s not DNSSEC", host->name);
    /*FALLTRHOUGH*/

  case DNS_NODATA:	/* no TLSA RR for this lookup */
  case DNS_NOMATCH:	/* no records at all for this lookup */
    return dane_required ? FAIL : FAIL_FORCED;

  default:
  case DNS_FAIL:
    return dane_required ? FAIL : DEFER;
  }
}
#endif	/*SUPPORT_DANE*/



/*************************************************
**************************************************
*             Stand-alone test program           *
**************************************************
*************************************************/

#ifdef STAND_ALONE

int main(int argc, char **cargv)
{
host_item h;
int whichrrs = HOST_FIND_BY_MX | HOST_FIND_BY_A | HOST_FIND_BY_AAAA;
BOOL byname = FALSE;
BOOL qualify_single = TRUE;
BOOL search_parents = FALSE;
BOOL request_dnssec = FALSE;
BOOL require_dnssec = FALSE;
uschar **argv = USS cargv;
uschar buffer[256];

disable_ipv6 = FALSE;
primary_hostname = US"";
store_init();
store_pool = POOL_MAIN;
debug_selector = D_host_lookup|D_interface;
debug_file = stdout;
debug_fd = fileno(debug_file);

printf("Exim stand-alone host functions test\n");

host_find_interfaces();
debug_selector = D_host_lookup | D_dns;

if (argc > 1) primary_hostname = argv[1];

/* So that debug level changes can be done first */

dns_init(qualify_single, search_parents, FALSE);

printf("Testing host lookup\n");
printf("> ");
while (Ufgets(buffer, 256, stdin) != NULL)
  {
  int rc;
  int len = Ustrlen(buffer);
  uschar *fully_qualified_name;

  while (len > 0 && isspace(buffer[len-1])) len--;
  buffer[len] = 0;

  if (Ustrcmp(buffer, "q") == 0) break;

  if (Ustrcmp(buffer, "byname") == 0) byname = TRUE;
  else if (Ustrcmp(buffer, "no_byname") == 0) byname = FALSE;
  else if (Ustrcmp(buffer, "a_only") == 0) whichrrs = HOST_FIND_BY_A | HOST_FIND_BY_AAAA;
  else if (Ustrcmp(buffer, "mx_only") == 0) whichrrs = HOST_FIND_BY_MX;
  else if (Ustrcmp(buffer, "srv_only") == 0) whichrrs = HOST_FIND_BY_SRV;
  else if (Ustrcmp(buffer, "srv+a") == 0)
    whichrrs = HOST_FIND_BY_SRV | HOST_FIND_BY_A | HOST_FIND_BY_AAAA;
  else if (Ustrcmp(buffer, "srv+mx") == 0)
    whichrrs = HOST_FIND_BY_SRV | HOST_FIND_BY_MX;
  else if (Ustrcmp(buffer, "srv+mx+a") == 0)
    whichrrs = HOST_FIND_BY_SRV | HOST_FIND_BY_MX | HOST_FIND_BY_A | HOST_FIND_BY_AAAA;
  else if (Ustrcmp(buffer, "qualify_single")    == 0) qualify_single = TRUE;
  else if (Ustrcmp(buffer, "no_qualify_single") == 0) qualify_single = FALSE;
  else if (Ustrcmp(buffer, "search_parents")    == 0) search_parents = TRUE;
  else if (Ustrcmp(buffer, "no_search_parents") == 0) search_parents = FALSE;
  else if (Ustrcmp(buffer, "request_dnssec")    == 0) request_dnssec = TRUE;
  else if (Ustrcmp(buffer, "no_request_dnssec") == 0) request_dnssec = FALSE;
  else if (Ustrcmp(buffer, "require_dnssec")    == 0) require_dnssec = TRUE;
  else if (Ustrcmp(buffer, "no_require_dnssec") == 0) require_dnssec = FALSE;
  else if (Ustrcmp(buffer, "test_harness") == 0)
    f.running_in_test_harness = !f.running_in_test_harness;
  else if (Ustrcmp(buffer, "ipv6") == 0) disable_ipv6 = !disable_ipv6;
  else if (Ustrcmp(buffer, "res_debug") == 0)
    {
    _res.options ^= RES_DEBUG;
    }
  else if (Ustrncmp(buffer, "retrans", 7) == 0)
    {
    (void)sscanf(CS(buffer+8), "%d", &dns_retrans);
    _res.retrans = dns_retrans;
    }
  else if (Ustrncmp(buffer, "retry", 5) == 0)
    {
    (void)sscanf(CS(buffer+6), "%d", &dns_retry);
    _res.retry = dns_retry;
    }
  else
    {
    int flags = whichrrs;
    dnssec_domains d;

    h.name = buffer;
    h.next = NULL;
    h.mx = MX_NONE;
    h.port = PORT_NONE;
    h.status = hstatus_unknown;
    h.why = hwhy_unknown;
    h.address = NULL;

    if (qualify_single) flags |= HOST_FIND_QUALIFY_SINGLE;
    if (search_parents) flags |= HOST_FIND_SEARCH_PARENTS;

    d.request = request_dnssec ? &h.name : NULL;
    d.require = require_dnssec ? &h.name : NULL;

    rc = byname
      ? host_find_byname(&h, NULL, flags, &fully_qualified_name, TRUE)
      : host_find_bydns(&h, NULL, flags, US"smtp", NULL, NULL,
			&d, &fully_qualified_name, NULL);

    switch (rc)
      {
      case HOST_FIND_FAILED:	printf("Failed\n");	break;
      case HOST_FIND_AGAIN:	printf("Again\n");	break;
      case HOST_FIND_SECURITY:	printf("Security\n");	break;
      case HOST_FOUND_LOCAL:	printf("Local\n");	break;
      }
    }

  printf("\n> ");
  }

printf("Testing host_aton\n");
printf("> ");
while (Ufgets(buffer, 256, stdin) != NULL)
  {
  int x[4];
  int len = Ustrlen(buffer);

  while (len > 0 && isspace(buffer[len-1])) len--;
  buffer[len] = 0;

  if (Ustrcmp(buffer, "q") == 0) break;

  len = host_aton(buffer, x);
  printf("length = %d ", len);
  for (int i = 0; i < len; i++)
    {
    printf("%04x ", (x[i] >> 16) & 0xffff);
    printf("%04x ", x[i] & 0xffff);
    }
  printf("\n> ");
  }

printf("\n");

printf("Testing host_name_lookup\n");
printf("> ");
while (Ufgets(buffer, 256, stdin) != NULL)
  {
  int len = Ustrlen(buffer);
  while (len > 0 && isspace(buffer[len-1])) len--;
  buffer[len] = 0;
  if (Ustrcmp(buffer, "q") == 0) break;
  sender_host_address = buffer;
  sender_host_name = NULL;
  sender_host_aliases = NULL;
  host_lookup_msg = US"";
  host_lookup_failed = FALSE;
  if (host_name_lookup() == FAIL)  /* Debug causes printing */
    printf("Lookup failed:%s\n", host_lookup_msg);
  printf("\n> ");
  }

printf("\n");

return 0;
}
#endif  /* STAND_ALONE */

/* vi: aw ai sw=2
*/
/* End of host.c */
