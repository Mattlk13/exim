/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) The Exim Maintainers 2020 - 2025 */
/* Copyright (c) University of Cambridge 1995 - 2018 */
/* See the file NOTICE for conditions of use and distribution. */
/* SPDX-License-Identifier: GPL-2.0-or-later */


/* Functions for handling string expansion. */


#include "exim.h"
#include <assert.h>

#ifdef MACRO_PREDEF
# include "macro_predef.h"
#endif

typedef unsigned esi_flags;
#define ESI_NOFLAGS		0
#define ESI_BRACE_ENDS		BIT(0)	/* expansion should stop at } */
#define ESI_HONOR_DOLLAR	BIT(1)	/* $ is meaningfull */
#define ESI_SKIPPING		BIT(2)	/* value will not be needed */
#define ESI_EXISTS_ONLY		BIT(3)	/* actual value not needed */

#ifdef STAND_ALONE
# ifndef SUPPORT_CRYPTEQ
#  define SUPPORT_CRYPTEQ
# endif
#endif	/*!STAND_ALONE*/

#ifdef SUPPORT_CRYPTEQ
# ifdef CRYPT_H
#  include <crypt.h>
# endif
# ifndef HAVE_CRYPT16
extern char* crypt16(char*, char*);
# endif
#endif

/* The handling of crypt16() is a mess. I will record below the analysis of the
mess that was sent to me. We decided, however, to make changing this very low
priority, because in practice people are moving away from the crypt()
algorithms nowadays, so it doesn't seem worth it.

<quote>
There is an algorithm named "crypt16" in Ultrix and Tru64.  It crypts
the first 8 characters of the password using a 20-round version of crypt
(standard crypt does 25 rounds).  It then crypts the next 8 characters,
or an empty block if the password is less than 9 characters, using a
20-round version of crypt and the same salt as was used for the first
block.  Characters after the first 16 are ignored.  It always generates
a 16-byte hash, which is expressed together with the salt as a string
of 24 base 64 digits.  Here are some links to peruse:

        http://cvs.pld.org.pl/pam/pamcrypt/crypt16.c?rev=1.2
        http://seclists.org/bugtraq/1999/Mar/0076.html

There's a different algorithm named "bigcrypt" in HP-UX, Digital Unix,
and OSF/1.  This is the same as the standard crypt if given a password
of 8 characters or less.  If given more, it first does the same as crypt
using the first 8 characters, then crypts the next 8 (the 9th to 16th)
using as salt the first two base 64 digits from the first hash block.
If the password is more than 16 characters then it crypts the 17th to 24th
characters using as salt the first two base 64 digits from the second hash
block.  And so on: I've seen references to it cutting off the password at
40 characters (5 blocks), 80 (10 blocks), or 128 (16 blocks).  Some links:

        http://cvs.pld.org.pl/pam/pamcrypt/bigcrypt.c?rev=1.2
        http://seclists.org/bugtraq/1999/Mar/0109.html
        http://h30097.www3.hp.com/docs/base_doc/DOCUMENTATION/HTML/AA-Q0R2D-
             TET1_html/sec.c222.html#no_id_208

Exim has something it calls "crypt16".  It will either use a native
crypt16 or its own implementation.  A native crypt16 will presumably
be the one that I called "crypt16" above.  The internal "crypt16"
function, however, is a two-block-maximum implementation of what I called
"bigcrypt".  The documentation matches the internal code.

I suspect that whoever did the "crypt16" stuff for Exim didn't realise
that crypt16 and bigcrypt were different things.

Exim uses the LDAP-style scheme identifier "{crypt16}" to refer
to whatever it is using under that name.  This unfortunately sets a
precedent for using "{crypt16}" to identify two incompatible algorithms
whose output can't be distinguished.  With "{crypt16}" thus rendered
ambiguous, I suggest you deprecate it and invent two new identifiers
for the two algorithms.

Both crypt16 and bigcrypt are very poor algorithms, btw.  Hashing parts
of the password separately means they can be cracked separately, so
the double-length hash only doubles the cracking effort instead of
squaring it.  I recommend salted SHA-1 ({SSHA}), or the Blowfish-based
bcrypt ({CRYPT}$2a$).
</quote>
*/



/*************************************************
*            Local statics and tables            *
*************************************************/

/* Table of item names, and corresponding switch numbers. The names must be in
alphabetical order. */

static uschar *item_table[] = {
  US"acl",
  US"authresults",
  US"certextract",
  US"dlfunc",
  US"env",
  US"extract",
  US"filter",
  US"hash",
  US"hmac",
  US"if",
#ifdef SUPPORT_I18N
  US"imapfolder",
#endif
  US"length",
  US"listextract",
  US"listquote",
  US"lookup",
  US"map",
  US"nhash",
  US"perl",
  US"prvs",
  US"prvscheck",
  US"readfile",
  US"readsocket",
  US"reduce",
  US"run",
  US"sg",
  US"sort",
#ifdef SUPPORT_SRS
  US"srs_encode",
#endif
  US"substr",
  US"tr" };

enum {
  EITEM_ACL,
  EITEM_AUTHRESULTS,
  EITEM_CERTEXTRACT,
  EITEM_DLFUNC,
  EITEM_ENV,
  EITEM_EXTRACT,
  EITEM_FILTER,
  EITEM_HASH,
  EITEM_HMAC,
  EITEM_IF,
#ifdef SUPPORT_I18N
  EITEM_IMAPFOLDER,
#endif
  EITEM_LENGTH,
  EITEM_LISTEXTRACT,
  EITEM_LISTQUOTE,
  EITEM_LOOKUP,
  EITEM_MAP,
  EITEM_NHASH,
  EITEM_PERL,
  EITEM_PRVS,
  EITEM_PRVSCHECK,
  EITEM_READFILE,
  EITEM_READSOCK,
  EITEM_REDUCE,
  EITEM_RUN,
  EITEM_SG,
  EITEM_SORT,
#ifdef SUPPORT_SRS
  EITEM_SRS_ENCODE,
#endif
  EITEM_SUBSTR,
  EITEM_TR };

/* Tables of operator names, and corresponding switch numbers. The names must be
in alphabetical order. There are two tables, because underscore is used in some
cases to introduce arguments, whereas for other it is part of the name. This is
an historical mis-design. */

static uschar * op_table_underscore[] = {
  US"from_utf8",
  US"local_part",
  US"quote_local_part",
  US"reverse_ip",
  US"time_eval",
  US"time_interval"
#ifdef SUPPORT_I18N
 ,US"utf8_domain_from_alabel",
  US"utf8_domain_to_alabel",
  US"utf8_localpart_from_alabel",
  US"utf8_localpart_to_alabel"
#endif
  };

enum {
  EOP_FROM_UTF8,
  EOP_LOCAL_PART,
  EOP_QUOTE_LOCAL_PART,
  EOP_REVERSE_IP,
  EOP_TIME_EVAL,
  EOP_TIME_INTERVAL
#ifdef SUPPORT_I18N
 ,EOP_UTF8_DOMAIN_FROM_ALABEL,
  EOP_UTF8_DOMAIN_TO_ALABEL,
  EOP_UTF8_LOCALPART_FROM_ALABEL,
  EOP_UTF8_LOCALPART_TO_ALABEL
#endif
  };

static uschar *op_table_main[] = {
  US"address",
  US"addresses",
  US"base32",
  US"base32d",
  US"base62",
  US"base62d",
  US"base64",
  US"base64d",
  US"domain",
  US"escape",
  US"escape8bit",
  US"eval",
  US"eval10",
  US"expand",
  US"h",
  US"hash",
  US"headerwrap",
  US"hex2b64",
  US"hexquote",
  US"ipv6denorm",
  US"ipv6norm",
  US"l",
  US"lc",
  US"length",
  US"listcount",
  US"listnamed",
  US"mask",
  US"md5",
  US"nh",
  US"nhash",
  US"quote",
  US"randint",
  US"rfc2047",
  US"rfc2047d",
  US"rxquote",
  US"s",
  US"sha1",
  US"sha2",
  US"sha256",
  US"sha3",
  US"stat",
  US"str2b64",
  US"strlen",
  US"substr",
  US"uc",
  US"utf8clean",
  US"xtextd",
  };

enum {
  EOP_ADDRESS =  nelem(op_table_underscore),
  EOP_ADDRESSES,
  EOP_BASE32,
  EOP_BASE32D,
  EOP_BASE62,
  EOP_BASE62D,
  EOP_BASE64,
  EOP_BASE64D,
  EOP_DOMAIN,
  EOP_ESCAPE,
  EOP_ESCAPE8BIT,
  EOP_EVAL,
  EOP_EVAL10,
  EOP_EXPAND,
  EOP_H,
  EOP_HASH,
  EOP_HEADERWRAP,
  EOP_HEX2B64,
  EOP_HEXQUOTE,
  EOP_IPV6DENORM,
  EOP_IPV6NORM,
  EOP_L,
  EOP_LC,
  EOP_LENGTH,
  EOP_LISTCOUNT,
  EOP_LISTNAMED,
  EOP_MASK,
  EOP_MD5,
  EOP_NH,
  EOP_NHASH,
  EOP_QUOTE,
  EOP_RANDINT,
  EOP_RFC2047,
  EOP_RFC2047D,
  EOP_RXQUOTE,
  EOP_S,
  EOP_SHA1,
  EOP_SHA2,
  EOP_SHA256,
  EOP_SHA3,
  EOP_STAT,
  EOP_STR2B64,
  EOP_STRLEN,
  EOP_SUBSTR,
  EOP_UC,
  EOP_UTF8CLEAN,
  EOP_XTEXTD,
  };


/* Table of condition names, and corresponding switch numbers. The names must
be in alphabetical order. */

static uschar *cond_table[] = {
  US"<",
  US"<=",
  US"=",
  US"==",     /* Backward compatibility */
  US">",
  US">=",
  US"acl",
  US"and",
  US"bool",
  US"bool_lax",
  US"crypteq",
  US"def",
  US"eq",
  US"eqi",
  US"exists",
  US"first_delivery",
  US"forall",
  US"forall_json",
  US"forall_jsons",
  US"forany",
  US"forany_json",
  US"forany_jsons",
  US"ge",
  US"gei",
  US"gt",
  US"gti",
#ifdef SUPPORT_SRS
  US"inbound_srs",
#endif
  US"inlist",
  US"inlisti",
  US"isip",
  US"isip4",
  US"isip6",
  US"ldapauth",
  US"le",
  US"lei",
  US"lt",
  US"lti",
  US"match",
  US"match_address",
  US"match_domain",
  US"match_ip",
  US"match_local_part",
  US"or",
  US"pam",
  US"pwcheck",
  US"queue_running",
  US"radius",
  US"saslauthd"
};

enum {
  ECOND_NUM_L,
  ECOND_NUM_LE,
  ECOND_NUM_E,
  ECOND_NUM_EE,
  ECOND_NUM_G,
  ECOND_NUM_GE,
  ECOND_ACL,
  ECOND_AND,
  ECOND_BOOL,
  ECOND_BOOL_LAX,
  ECOND_CRYPTEQ,
  ECOND_DEF,
  ECOND_STR_EQ,
  ECOND_STR_EQI,
  ECOND_EXISTS,
  ECOND_FIRST_DELIVERY,
  ECOND_FORALL,
  ECOND_FORALL_JSON,
  ECOND_FORALL_JSONS,
  ECOND_FORANY,
  ECOND_FORANY_JSON,
  ECOND_FORANY_JSONS,
  ECOND_STR_GE,
  ECOND_STR_GEI,
  ECOND_STR_GT,
  ECOND_STR_GTI,
#ifdef SUPPORT_SRS
  ECOND_INBOUND_SRS,
#endif
  ECOND_INLIST,
  ECOND_INLISTI,
  ECOND_ISIP,
  ECOND_ISIP4,
  ECOND_ISIP6,
  ECOND_LDAPAUTH,
  ECOND_STR_LE,
  ECOND_STR_LEI,
  ECOND_STR_LT,
  ECOND_STR_LTI,
  ECOND_MATCH,
  ECOND_MATCH_ADDRESS,
  ECOND_MATCH_DOMAIN,
  ECOND_MATCH_IP,
  ECOND_MATCH_LOCAL_PART,
  ECOND_OR,
  ECOND_PAM,
  ECOND_PWCHECK,
  ECOND_QUEUE_RUNNING,
  ECOND_RADIUS,
  ECOND_SASLAUTHD
};


/* Type for entries pointing to address/length pairs. Not currently
in use.

typedef struct {
  uschar **address;
  int  *length;
} alblock;
*/

typedef uschar * stringptr_fn_t(void);
static uschar * fn_queue_size(void);
static uschar * fn_recipients(void);
static uschar * fn_recipients_list(void);
#ifdef MACRO_PREDEF
# define NOT_MPDF(str) NULL
#else
# define NOT_MPDF(str) str
#endif

/* This table must be kept in alphabetical order. */

static var_entry var_table[] = {
  /* WARNING: Do not invent variables whose names start acl_c or acl_m because
     they will be confused with user-creatable ACL variables. */
  { "acl_arg1",            vtype_stringptr,   &acl_arg[0] },
  { "acl_arg2",            vtype_stringptr,   &acl_arg[1] },
  { "acl_arg3",            vtype_stringptr,   &acl_arg[2] },
  { "acl_arg4",            vtype_stringptr,   &acl_arg[3] },
  { "acl_arg5",            vtype_stringptr,   &acl_arg[4] },
  { "acl_arg6",            vtype_stringptr,   &acl_arg[5] },
  { "acl_arg7",            vtype_stringptr,   &acl_arg[6] },
  { "acl_arg8",            vtype_stringptr,   &acl_arg[7] },
  { "acl_arg9",            vtype_stringptr,   &acl_arg[8] },
  { "acl_narg",            vtype_int,         &acl_narg },
  { "acl_verify_message",  vtype_stringptr,   &acl_verify_message },
  { "address_data",        vtype_stringptr,   &deliver_address_data },
  { "address_file",        vtype_stringptr,   &address_file },
  { "address_pipe",        vtype_stringptr,   &address_pipe },
#ifdef EXPERIMENTAL_ARC
  { "arc_domains",         vtype_module,	US"arc" },
  { "arc_oldest_pass",     vtype_module,	US"arc" },
  { "arc_state",           vtype_module,	US"arc" },
  { "arc_state_reason",    vtype_module,	US"arc" },
#endif
  { "atrn_host",	   vtype_stringptr,   &atrn_host },
  { "atrn_mode",	   vtype_stringptr,   &atrn_mode },
  { "authenticated_fail_id",vtype_stringptr,  &authenticated_fail_id },
  { "authenticated_id",    vtype_stringptr,   &authenticated_id },
  { "authenticated_sender",vtype_stringptr,   &authenticated_sender },
  { "authentication_failed",vtype_int,        &authentication_failed },
#ifdef WITH_CONTENT_SCAN
  { "av_failed",           vtype_int,         &av_failed },
#endif
#ifdef EXPERIMENTAL_BRIGHTMAIL
  { "bmi_alt_location",    vtype_stringptr,   &bmi_alt_location },
  { "bmi_base64_tracker_verdict", vtype_stringptr, &bmi_base64_tracker_verdict },
  { "bmi_base64_verdict",  vtype_stringptr,   &bmi_base64_verdict },
  { "bmi_deliver",         vtype_int,         &bmi_deliver },
#endif
  { "body_linecount",      vtype_int,         &body_linecount },
  { "body_zerocount",      vtype_int,         &body_zerocount },
  { "bounce_recipient",    vtype_stringptr,   &bounce_recipient },
  { "bounce_return_size_limit", vtype_int,    &bounce_return_size_limit },
  { "caller_gid",          vtype_gid,         &real_gid },
  { "caller_uid",          vtype_uid,         &real_uid },
  { "callout_address",     vtype_stringptr,   &callout_address },
  { "compile_date",        vtype_stringptr,   &version_date },
  { "compile_number",      vtype_stringptr,   &version_cnumber },
  { "config_dir",          vtype_stringptr,   &config_main_directory },
  { "config_file",         vtype_stringptr,   &config_main_filename },
  { "connection_id",       vtype_stringptr,   &connection_id },
  { "csa_status",          vtype_stringptr,   &csa_status },
#ifdef EXPERIMENTAL_DCC
  { "dcc_header",          vtype_stringptr,   &dcc_header },
  { "dcc_result",          vtype_stringptr,   &dcc_result },
#endif
#ifndef DISABLE_DKIM
  { "dkim_algo",           vtype_module,	US"dkim" },
  { "dkim_bodylength",     vtype_module,	US"dkim" },
  { "dkim_canon_body",     vtype_module,	US"dkim" },
  { "dkim_canon_headers",  vtype_module,	US"dkim" },
  { "dkim_copiedheaders",  vtype_module,	US"dkim" },
  { "dkim_created",        vtype_module,	US"dkim" },
  { "dkim_cur_signer",     vtype_module,	US"dkim" },
  { "dkim_domain",         vtype_module,	US"dkim" },
  { "dkim_expires",        vtype_module,	US"dkim" },
  { "dkim_headernames",    vtype_module,	US"dkim" },
  { "dkim_identity",       vtype_module,	US"dkim" },
  { "dkim_key_granularity",vtype_module,	US"dkim" },
  { "dkim_key_length",     vtype_module,	US"dkim" },
  { "dkim_key_nosubdomains",vtype_module,	US"dkim" },
  { "dkim_key_notes",      vtype_module,	US"dkim" },
  { "dkim_key_srvtype",    vtype_module,	US"dkim" },
  { "dkim_key_testing",    vtype_module,	US"dkim" },
  { "dkim_selector",       vtype_module,	US"dkim" },
  { "dkim_signers",        vtype_module,	US"dkim" },
  { "dkim_verify_reason",  vtype_module,	US"dkim" },
  { "dkim_verify_signers", vtype_module,	US"dkim" },
  { "dkim_verify_status",  vtype_module,	US"dkim" },
#endif
#ifdef SUPPORT_DMARC
  { "dmarc_alignment_dkim",vtype_module,	US"dmarc" },
  { "dmarc_alignment_spf", vtype_module,	US"dmarc" },
  { "dmarc_domain_policy", vtype_module,	US"dmarc" },
  { "dmarc_status",        vtype_module,	US"dmarc" },
  { "dmarc_status_text",   vtype_module,	US"dmarc" },
  { "dmarc_used_domain",   vtype_module,	US"dmarc" },
#endif
  { "dnslist_domain",      vtype_stringptr,   &dnslist_domain },
  { "dnslist_matched",     vtype_stringptr,   &dnslist_matched },
  { "dnslist_text",        vtype_stringptr,   &dnslist_text },
  { "dnslist_value",       vtype_stringptr,   &dnslist_value },
  { "domain",              vtype_stringptr,   &deliver_domain },
  { "domain_data",         vtype_stringptr,   &deliver_domain_data },
#ifndef DISABLE_EVENT
  { "event_data",          vtype_stringptr,   &event_data },

  /*XXX want to use generic vars for as many of these as possible*/
  { "event_defer_errno",   vtype_int,         &event_defer_errno },

  { "event_name",          vtype_stringptr,   &event_name },
#endif
  { "exim_gid",            vtype_gid,         &exim_gid },
  { "exim_path",           vtype_stringptr,   &exim_path },
  { "exim_uid",            vtype_uid,         &exim_uid },
  { "exim_version",        vtype_stringptr,   &version_string },
  { "headers_added",       vtype_string_func, NOT_MPDF((void *) &fn_hdrs_added) },
  { "home",                vtype_stringptr,   &deliver_home },
  { "host",                vtype_stringptr,   &deliver_host },
  { "host_address",        vtype_stringptr,   &deliver_host_address },
  { "host_data",           vtype_stringptr,   &host_data },
  { "host_lookup_deferred",vtype_int,         &host_lookup_deferred },
  { "host_lookup_failed",  vtype_int,         &host_lookup_failed },
  { "host_port",           vtype_int,         &deliver_host_port },
  { "initial_cwd",         vtype_stringptr,   &initial_cwd },
  { "inode",               vtype_ino,         &deliver_inode },
  { "interface_address",   vtype_stringptr,   &interface_address },
  { "interface_port",      vtype_int,         &interface_port },
  { "item",                vtype_stringptr,   &iterate_item },
#ifdef LOOKUP_LDAP
  { "ldap_dn",             vtype_stringptr,   &eldap_dn },
#endif
  { "load_average",        vtype_load_avg,    NULL },
  { "local_part",          vtype_stringptr,   &deliver_localpart },
  { "local_part_data",     vtype_stringptr,   &deliver_localpart_data },
  { "local_part_prefix",   vtype_stringptr,   &deliver_localpart_prefix },
  { "local_part_prefix_v", vtype_stringptr,   &deliver_localpart_prefix_v },
  { "local_part_suffix",   vtype_stringptr,   &deliver_localpart_suffix },
  { "local_part_suffix_v", vtype_stringptr,   &deliver_localpart_suffix_v },
#ifdef HAVE_LOCAL_SCAN
  { "local_scan_data",     vtype_stringptr,   &local_scan_data },
#endif
  { "local_user_gid",      vtype_gid,         &local_user_gid },
  { "local_user_uid",      vtype_uid,         &local_user_uid },
  { "localhost_number",    vtype_int,         &host_number },
  { "log_inodes",          vtype_pinodes,     (void *)FALSE },
  { "log_space",           vtype_pspace,      (void *)FALSE },
  { "lookup_dnssec_authenticated",vtype_stringptr,&lookup_dnssec_authenticated},
  { "mailstore_basename",  vtype_stringptr,   &mailstore_basename },
#ifdef WITH_CONTENT_SCAN
  { "malware_name",        vtype_stringptr,   &malware_name },
#endif
  { "max_received_linelength", vtype_int,     &max_received_linelength },
  { "message_age",         vtype_int,         &message_age },
  { "message_body",        vtype_msgbody,     &message_body },
  { "message_body_end",    vtype_msgbody_end, &message_body_end },
  { "message_body_size",   vtype_int,         &message_body_size },
  { "message_exim_id",     vtype_stringptr,   &message_id },
  { "message_headers",     vtype_msgheaders,  NULL },
  { "message_headers_raw", vtype_msgheaders_raw, NULL },
  { "message_id",          vtype_stringptr,   &message_id },
  { "message_linecount",   vtype_int,         &message_linecount },
  { "message_size",        vtype_int,         &message_size },
#ifdef SUPPORT_I18N
  { "message_smtputf8",    vtype_bool,        &message_smtputf8 },
#endif
#ifdef WITH_CONTENT_SCAN
  { "mime_anomaly_level",  vtype_int,         &mime_anomaly_level },
  { "mime_anomaly_text",   vtype_stringptr,   &mime_anomaly_text },
  { "mime_boundary",       vtype_stringptr,   &mime_boundary },
  { "mime_charset",        vtype_stringptr,   &mime_charset },
  { "mime_content_description", vtype_stringptr, &mime_content_description },
  { "mime_content_disposition", vtype_stringptr, &mime_content_disposition },
  { "mime_content_id",     vtype_stringptr,   &mime_content_id },
  { "mime_content_size",   vtype_int,         &mime_content_size },
  { "mime_content_transfer_encoding",vtype_stringptr, &mime_content_transfer_encoding },
  { "mime_content_type",   vtype_stringptr,   &mime_content_type },
  { "mime_decoded_filename", vtype_stringptr, &mime_decoded_filename },
  { "mime_filename",       vtype_stringptr,   &mime_filename },
  { "mime_is_coverletter", vtype_int,         &mime_is_coverletter },
  { "mime_is_multipart",   vtype_int,         &mime_is_multipart },
  { "mime_is_rfc822",      vtype_int,         &mime_is_rfc822 },
  { "mime_part_count",     vtype_int,         &mime_part_count },
#endif
  { "n0",                  vtype_filter_int,  &filter_n[0] },
  { "n1",                  vtype_filter_int,  &filter_n[1] },
  { "n2",                  vtype_filter_int,  &filter_n[2] },
  { "n3",                  vtype_filter_int,  &filter_n[3] },
  { "n4",                  vtype_filter_int,  &filter_n[4] },
  { "n5",                  vtype_filter_int,  &filter_n[5] },
  { "n6",                  vtype_filter_int,  &filter_n[6] },
  { "n7",                  vtype_filter_int,  &filter_n[7] },
  { "n8",                  vtype_filter_int,  &filter_n[8] },
  { "n9",                  vtype_filter_int,  &filter_n[9] },
  { "original_domain",     vtype_stringptr,   &deliver_domain_orig },
  { "original_local_part", vtype_stringptr,   &deliver_localpart_orig },
  { "originator_gid",      vtype_gid,         &originator_gid },
  { "originator_uid",      vtype_uid,         &originator_uid },
  { "parent_domain",       vtype_stringptr,   &deliver_domain_parent },
  { "parent_local_part",   vtype_stringptr,   &deliver_localpart_parent },
  { "pid",                 vtype_pid,         NULL },
#ifndef DISABLE_PRDR
  { "prdr_requested",      vtype_bool,        &prdr_requested },
#endif
  { "primary_hostname",    vtype_stringptr,   &primary_hostname },
#if defined(SUPPORT_PROXY) || defined(SUPPORT_SOCKS)
  { "proxy_external_address",vtype_stringptr, &proxy_external_address },
  { "proxy_external_port", vtype_int,         &proxy_external_port },
  { "proxy_local_address", vtype_stringptr,   &proxy_local_address },
  { "proxy_local_port",    vtype_int,         &proxy_local_port },
  { "proxy_session",       vtype_bool,        &proxy_session },
#endif
  { "prvscheck_address",   vtype_stringptr,   &prvscheck_address },
  { "prvscheck_keynum",    vtype_stringptr,   &prvscheck_keynum },
  { "prvscheck_result",    vtype_stringptr,   &prvscheck_result },
  { "qualify_domain",      vtype_stringptr,   &qualify_domain_sender },
  { "qualify_recipient",   vtype_stringptr,   &qualify_domain_recipient },
  { "queue_name",          vtype_stringptr,   &queue_name },
  { "queue_size",          vtype_string_func, (void *) &fn_queue_size },
  { "rcpt_count",          vtype_int,         &rcpt_count },
  { "rcpt_defer_count",    vtype_int,         &rcpt_defer_count },
  { "rcpt_fail_count",     vtype_int,         &rcpt_fail_count },
  { "received_count",      vtype_int,         &received_count },
  { "received_for",        vtype_stringptr,   &received_for },
  { "received_ip_address", vtype_stringptr,   &interface_address },
  { "received_port",       vtype_int,         &interface_port },
  { "received_protocol",   vtype_stringptr,   &received_protocol },
  { "received_time",       vtype_int,         &received_time.tv_sec },
  { "recipient_data",      vtype_stringptr,   &recipient_data },
  { "recipient_verify_failure",vtype_stringptr,&recipient_verify_failure },
  { "recipients",          vtype_string_func, (void *) &fn_recipients },
  { "recipients_count",    vtype_int,         &recipients_count },
  { "recipients_list",     vtype_string_func, (void *) &fn_recipients_list },
  { "regex_cachesize",     vtype_int,         &regex_cachesize },/* undocumented; devel observability */
#ifdef WITH_CONTENT_SCAN
  { "regex_match_string",  vtype_stringptr,   &regex_match_string },
#endif
  { "reply_address",       vtype_reply,       NULL },
  { "return_path",         vtype_stringptr,   &return_path },
  { "return_size_limit",   vtype_int,         &bounce_return_size_limit },
  { "router_name",         vtype_stringptr,   &router_name },
  { "runrc",               vtype_int,         &runrc },
  { "self_hostname",       vtype_stringptr,   &self_hostname },
  { "sender_address",      vtype_stringptr,   &sender_address },
  { "sender_address_data", vtype_stringptr,   &sender_address_data },
  { "sender_address_domain", vtype_domain,    &sender_address },
  { "sender_address_local_part", vtype_localpart, &sender_address },
  { "sender_data",         vtype_stringptr,   &sender_data },
  { "sender_fullhost",     vtype_stringptr,   &sender_fullhost },
  { "sender_helo_dnssec",  vtype_bool,        &sender_helo_dnssec },
  { "sender_helo_name",    vtype_stringptr,   &sender_helo_name },
  { "sender_helo_verified",vtype_string_func, NOT_MPDF((void *) &sender_helo_verified_boolstr) },
  { "sender_host_address", vtype_stringptr,   &sender_host_address },
  { "sender_host_authenticated",vtype_stringptr, &sender_host_authenticated },
  { "sender_host_dnssec",  vtype_bool,        &sender_host_dnssec },
  { "sender_host_name",    vtype_host_lookup, NULL },
  { "sender_host_port",    vtype_int,         &sender_host_port },
  { "sender_ident",        vtype_stringptr,   &sender_ident },
  { "sender_rate",         vtype_stringptr,   &sender_rate },
  { "sender_rate_limit",   vtype_stringptr,   &sender_rate_limit },
  { "sender_rate_period",  vtype_stringptr,   &sender_rate_period },
  { "sender_rcvhost",      vtype_stringptr,   &sender_rcvhost },
  { "sender_verify_failure",vtype_stringptr,  &sender_verify_failure },
  { "sending_ip_address",  vtype_stringptr,   &sending_ip_address },
  { "sending_port",        vtype_int,         &sending_port },
  { "smtp_active_hostname", vtype_stringptr,  &smtp_active_hostname },
  { "smtp_command",        vtype_stringptr,   &smtp_cmd_buffer },
  { "smtp_command_argument", vtype_stringptr, &smtp_cmd_argument },
  { "smtp_command_history", vtype_string_func, NOT_MPDF((void *) &smtp_cmd_hist) },
  { "smtp_count_at_connection_start", vtype_int, &smtp_accept_count },
  { "smtp_notquit_reason", vtype_stringptr,   &smtp_notquit_reason },
  { "sn0",                 vtype_filter_int,  &filter_sn[0] },
  { "sn1",                 vtype_filter_int,  &filter_sn[1] },
  { "sn2",                 vtype_filter_int,  &filter_sn[2] },
  { "sn3",                 vtype_filter_int,  &filter_sn[3] },
  { "sn4",                 vtype_filter_int,  &filter_sn[4] },
  { "sn5",                 vtype_filter_int,  &filter_sn[5] },
  { "sn6",                 vtype_filter_int,  &filter_sn[6] },
  { "sn7",                 vtype_filter_int,  &filter_sn[7] },
  { "sn8",                 vtype_filter_int,  &filter_sn[8] },
  { "sn9",                 vtype_filter_int,  &filter_sn[9] },
#ifdef WITH_CONTENT_SCAN
  { "spam_action",         vtype_stringptr,   &spam_action },
  { "spam_bar",            vtype_stringptr,   &spam_bar },
  { "spam_report",         vtype_stringptr,   &spam_report },
  { "spam_score",          vtype_stringptr,   &spam_score },
  { "spam_score_int",      vtype_stringptr,   &spam_score_int },
#endif
#ifdef EXIM_HAVE_SPF
  { "spf_guess",           vtype_module,	US"spf" },
  { "spf_header_comment",  vtype_module,	US"spf" },
  { "spf_received",        vtype_module,	US"spf" },
  { "spf_result",          vtype_module,	US"spf" },
  { "spf_result_guessed",  vtype_module,	US"spf" },
  { "spf_smtp_comment",    vtype_module,	US"spf" },
#endif
  { "spool_directory",     vtype_stringptr,   &spool_directory },
  { "spool_inodes",        vtype_pinodes,     (void *)TRUE },
  { "spool_space",         vtype_pspace,      (void *)TRUE },
#ifdef SUPPORT_SRS
  { "srs_recipient",       vtype_stringptr,   &srs_recipient },
#endif
  { "thisaddress",         vtype_stringptr,   &filter_thisaddress },

  /* The non-(in,out) variables are now deprecated */
  { "tls_bits",		   vtype_int,         NOT_MPDF(&tls_in.bits) },
  { "tls_certificate_verified", vtype_int,    NOT_MPDF(&tls_in.certificate_verified) },
  { "tls_cipher",          vtype_stringptr,   NOT_MPDF(&tls_in.cipher) },

  { "tls_in_bits",         vtype_int,         NOT_MPDF(&tls_in.bits) },
  { "tls_in_certificate_verified", vtype_int, NOT_MPDF(&tls_in.certificate_verified) },
  { "tls_in_cipher",       vtype_stringptr,   NOT_MPDF(&tls_in.cipher) },
  { "tls_in_cipher_std",   vtype_stringptr,   NOT_MPDF(&tls_in.cipher_stdname) },
  { "tls_in_ocsp",         vtype_int,         NOT_MPDF(&tls_in.ocsp) },
  { "tls_in_ourcert",      vtype_cert,        NOT_MPDF(&tls_in.ourcert) },
  { "tls_in_peercert",     vtype_cert,        NOT_MPDF(&tls_in.peercert) },
  { "tls_in_peerdn",       vtype_stringptr,   NOT_MPDF(&tls_in.peerdn) },
#ifndef DISABLE_TLS_RESUME
  { "tls_in_resumption",   vtype_int,         NOT_MPDF(&tls_in.resumption) },
#endif
#ifndef DISABLE_TLS
  { "tls_in_sni",          vtype_stringptr,   NOT_MPDF(&tls_in.sni) },
#endif
  { "tls_in_ver",          vtype_stringptr,   NOT_MPDF(&tls_in.ver) },
  { "tls_out_bits",        vtype_int,         NOT_MPDF(&tls_out.bits) },
  { "tls_out_certificate_verified", vtype_int,NOT_MPDF(&tls_out.certificate_verified) },
  { "tls_out_cipher",      vtype_stringptr,   NOT_MPDF(&tls_out.cipher) },
  { "tls_out_cipher_std",  vtype_stringptr,   NOT_MPDF(&tls_out.cipher_stdname) },
#ifdef SUPPORT_DANE
  { "tls_out_dane",        vtype_bool,        NOT_MPDF(&tls_out.dane_verified) },
#endif
  { "tls_out_ocsp",        vtype_int,         NOT_MPDF(&tls_out.ocsp) },
  { "tls_out_ourcert",     vtype_cert,        NOT_MPDF(&tls_out.ourcert) },
  { "tls_out_peercert",    vtype_cert,        NOT_MPDF(&tls_out.peercert) },
  { "tls_out_peerdn",      vtype_stringptr,   NOT_MPDF(&tls_out.peerdn) },
#ifndef DISABLE_TLS_RESUME
  { "tls_out_resumption",  vtype_int,         NOT_MPDF(&tls_out.resumption) },
#endif
#ifndef DISABLE_TLS
  { "tls_out_sni",         vtype_stringptr,   NOT_MPDF(&tls_out.sni) },
#endif
#ifdef SUPPORT_DANE
  { "tls_out_tlsa_usage",  vtype_int,         NOT_MPDF(&tls_out.tlsa_usage) },
#endif
  { "tls_out_ver",         vtype_stringptr,   NOT_MPDF(&tls_out.ver) },

  { "tls_peerdn",          vtype_stringptr,   NOT_MPDF(&tls_in.peerdn) },	/* mind the alphabetical order! */
#ifndef DISABLE_TLS
  { "tls_sni",             vtype_stringptr,   NOT_MPDF(&tls_in.sni) },	/* mind the alphabetical order! */
#endif

  { "tod_bsdinbox",        vtype_todbsdin,    NULL },
  { "tod_epoch",           vtype_tode,        NULL },
  { "tod_epoch_l",         vtype_todel,       NULL },
  { "tod_full",            vtype_todf,        NULL },
  { "tod_log",             vtype_todl,        NULL },
  { "tod_logfile",         vtype_todlf,       NULL },
  { "tod_zone",            vtype_todzone,     NULL },
  { "tod_zulu",            vtype_todzulu,     NULL },
  { "transport_name",      vtype_stringptr,   &transport_name },
  { "value",               vtype_stringptr,   &lookup_value },
  { "verify_mode",         vtype_stringptr,   &verify_mode },
  { "version_number",      vtype_stringptr,   &version_string },
  { "warn_message_delay",  vtype_stringptr,   &warnmsg_delay },
  { "warn_message_recipient",vtype_stringptr, &warnmsg_recipients },
  { "warn_message_recipients",vtype_stringptr,&warnmsg_recipients },
  { "warnmsg_delay",       vtype_stringptr,   &warnmsg_delay },
  { "warnmsg_recipient",   vtype_stringptr,   &warnmsg_recipients },
  { "warnmsg_recipients",  vtype_stringptr,   &warnmsg_recipients }
};

#ifdef MACRO_PREDEF

/* dummies */
uschar * fn_arc_domains(void) {return NULL;}
uschar * fn_hdrs_added(void) {return NULL;}
uschar * fn_queue_size(void) {return NULL;}
uschar * fn_recipients(void) {return NULL;}
uschar * fn_recipients_list(void) {return NULL;}



static void
expansion_items(void)
{
uschar buf[64];
for (int i = 0; i < nelem(item_table); i++)
  {
  spf(buf, sizeof(buf), CUS"_EXP_ITEM_%T", item_table[i]);
  builtin_macro_create(buf);
  }
}
static void
expansion_operators(void)
{
uschar buf[64];
for (int i = 0; i < nelem(op_table_underscore); i++)
  {
  spf(buf, sizeof(buf), CUS"_EXP_OP_%T", op_table_underscore[i]);
  builtin_macro_create(buf);
  }
for (int i = 0; i < nelem(op_table_main); i++)
  {
  spf(buf, sizeof(buf), CUS"_EXP_OP_%T", op_table_main[i]);
  builtin_macro_create(buf);
  }
}
static void
expansion_conditions(void)
{
uschar buf[64];
for (int i = 0; i < nelem(cond_table); i++)
  {
  spf(buf, sizeof(buf), CUS"_EXP_COND_%T", cond_table[i]);
  builtin_macro_create(buf);
  }
}
static void
expansion_variables(void)
{
uschar buf[64];
for (int i = 0; i < nelem(var_table); i++)
  {
  spf(buf, sizeof(buf), CUS"_EXP_VAR_%T", var_table[i].name);
  builtin_macro_create(buf);
  }
}

void
expansions(void)
{
expansion_items();
expansion_operators();
expansion_conditions();
expansion_variables();
}

#else	/*!MACRO_PREDEF*/

static uschar var_buffer[256];
static BOOL malformed_header;

/* For textual hashes */

static const char *hashcodes = "abcdefghijklmnopqrtsuvwxyz"
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "0123456789";

enum { HMAC_MD5, HMAC_SHA1 };

/* For numeric hashes */

static unsigned int prime[] = {
  2,   3,   5,   7,  11,  13,  17,  19,  23,  29,
 31,  37,  41,  43,  47,  53,  59,  61,  67,  71,
 73,  79,  83,  89,  97, 101, 103, 107, 109, 113};

/* For printing modes in symbolic form */

static uschar *mtable_normal[] =
  { US"---", US"--x", US"-w-", US"-wx", US"r--", US"r-x", US"rw-", US"rwx" };

static uschar *mtable_setid[] =
  { US"--S", US"--s", US"-wS", US"-ws", US"r-S", US"r-s", US"rwS", US"rws" };

static uschar *mtable_sticky[] =
  { US"--T", US"--t", US"-wT", US"-wt", US"r-T", US"r-t", US"rwT", US"rwt" };

/* flags for find_header() */
#define FH_EXISTS_ONLY	BIT(0)
#define FH_WANT_RAW	BIT(1)
#define FH_WANT_LIST	BIT(2)

/* Recursively called function */
static uschar *expand_string_internal(const uschar *, esi_flags, const uschar **, BOOL *, BOOL *);
static int_eximarith_t expanded_string_integer(const uschar *, BOOL);


/*************************************************
*           Tables for UTF-8 support             *
*************************************************/

/* Table of the number of extra characters, indexed by the first character
masked with 0x3f. The highest number for a valid UTF-8 character is in fact
0x3d. */

static uschar utf8_table1[] = {
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  3,3,3,3,3,3,3,3,4,4,4,4,5,5,5,5 };

/* These are the masks for the data bits in the first byte of a character,
indexed by the number of additional bytes. */

static int utf8_table2[] = { 0xff, 0x1f, 0x0f, 0x07, 0x03, 0x01};

/* Get the next UTF-8 character, advancing the pointer. */

#define GETUTF8INC(c, ptr) \
  c = *ptr++; \
  if ((c & 0xc0) == 0xc0) \
    { \
    int a = utf8_table1[c & 0x3f];  /* Number of additional bytes */ \
    int s = 6*a; \
    c = (c & utf8_table2[a]) << s; \
    while (a-- > 0) \
      { \
      s -= 6; \
      c |= (*ptr++ & 0x3f) << s; \
      } \
    }



static uschar * base32_chars = US"abcdefghijklmnopqrstuvwxyz234567";

/*************************************************
*           Binary chop search on a table        *
*************************************************/

/* This is used for matching expansion items and operators.

Arguments:
  name        the name that is being sought
  table       the table to search
  table_size  the number of items in the table

Returns:      the offset in the table, or -1
*/

static int
chop_match(const uschar * name, uschar ** table, int table_size)
{
uschar ** bot = table;
uschar ** top = table + table_size;

while (top > bot)
  {
  uschar **mid = bot + (top - bot)/2;
  int c = Ustrcmp(name, *mid);
  if (c == 0) return mid - table;
  if (c > 0) bot = mid + 1; else top = mid;
  }

return -1;
}



/*************************************************
*          Check a condition string              *
*************************************************/

/* This function is called to expand a string, and test the result for a "true"
or "false" value. Failure of the expansion yields FALSE; logged unless it was a
forced fail or lookup defer.

We used to release all store used, but this is not not safe due
to ${dlfunc } and ${acl }.  In any case expand_string_internal()
is reasonably careful to release what it can.

The actual false-value tests should be replicated for ECOND_BOOL_LAX.

Arguments:
  condition     the condition string
  m1            text to be incorporated in panic error
  m2            ditto

Returns:        TRUE if condition is met, FALSE if not
*/

BOOL
expand_check_condition(const uschar * condition,
  const uschar * m1, const uschar * m2)
{
const uschar * ss = expand_string(condition);
if (!ss)
  {
  if (!f.expand_string_forcedfail && !f.search_find_defer)
    log_write(0, LOG_MAIN|LOG_PANIC, "failed to expand condition %q "
      "for %s %s: %s", condition, m1, m2, expand_string_message);
  return FALSE;
  }
return *ss && Ustrcmp(ss, "0") != 0 && strcmpic(ss, US"no") != 0 &&
  strcmpic(ss, US"false") != 0;
}




/*************************************************
*        Pseudo-random number generation         *
*************************************************/

/* Pseudo-random number generation.  The result is not "expected" to be
cryptographically strong but not so weak that someone will shoot themselves
in the foot using it as a nonce in some email header scheme or whatever
weirdness they'll twist this into.  The result should ideally handle fork().

However, if we're stuck unable to provide this, then we'll fall back to
appallingly bad randomness.

If DISABLE_TLS is not defined then this will not be used except as an emergency
fallback.

Arguments:
  max       range maximum
Returns     a random number in range [0, max-1]
*/

#ifndef DISABLE_TLS
# define vaguely_random_number vaguely_random_number_fallback
#endif
int
vaguely_random_number(int max)
{
#ifndef DISABLE_TLS
# undef vaguely_random_number
#endif
static pid_t pid = 0;
pid_t p2;

if ((p2 = getpid()) != pid)
  {
  if (pid != 0)
    {

#ifdef HAVE_ARC4RANDOM
    /* cryptographically strong randomness, common on *BSD platforms, not
    so much elsewhere.  Alas. */
# ifndef NOT_HAVE_ARC4RANDOM_STIR
    arc4random_stir();
# endif
#elif defined(HAVE_SRANDOM) || defined(HAVE_SRANDOMDEV)
# ifdef HAVE_SRANDOMDEV
    /* uses random(4) for seeding */
    srandomdev();
# else
    {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srandom(tv.tv_sec | tv.tv_usec | getpid());
    }
# endif
#else
    /* Poor randomness and no seeding here */
#endif

    }
  pid = p2;
  }

#ifdef HAVE_ARC4RANDOM
return arc4random() % max;
#elif defined(HAVE_SRANDOM) || defined(HAVE_SRANDOMDEV)
return random() % max;
#else
/* This one returns a 16-bit number, definitely not crypto-strong */
return random_number(max);
#endif
}




/*************************************************
*             Pick out a name from a string      *
*************************************************/

/* If the name is too long, it is silently truncated.

In theory all callers present a non-tainted string, so the non-tracking
copy into the buffer is ok.

Arguments:
  name      points to a buffer into which to put the name
  max       is the length of the buffer
  s         points to the first alphabetic character of the name
  extras    chars other than alphanumerics to permit

Returns:    pointer to the first character after the name

Note: The test for *s != 0 in the while loop is necessary because
Ustrchr() yields non-NULL if the character is zero (which is not something
I expected). */

static const uschar *
read_name(uschar * name, int max, const uschar * s, const uschar * extras)
{
int ptr = 0;
if (f.running_in_test_harness) assert(!is_tainted(s));
while (*s && (isalnum(*s) || Ustrchr(extras, *s) != NULL))
  {
  if (ptr < max-1) name[ptr++] = *s;
  s++;
  }
name[ptr] = 0;
return s;
}



/*************************************************
*     Pick out the rest of a header name         *
*************************************************/

/* A variable name starting $header_ (or just $h_ for those who like
abbreviations) might not be the complete header name because headers can
contain any printing characters in their names, except ':'. This function is
called to read the rest of the name, chop h[eader]_ off the front, and put ':'
on the end, if the name was terminated by white space.

Arguments:
  name      points to a buffer in which the name read so far exists
  max       is the length of the buffer
  s         points to the first character after the name so far, i.e. the
            first non-alphameric character after $header_xxxxx

Returns:    a pointer to the first character after the header name
*/

static const uschar *
read_header_name(uschar *name, int max, const uschar *s)
{
int prelen = Ustrchr(name, '_') - name + 1;
int ptr = Ustrlen(name) - prelen;
if (ptr > 0) memmove(name, name+prelen, ptr);
while (mac_isgraph(*s) && *s != ':')
  {
  if (ptr < max-1) name[ptr++] = *s;
  s++;
  }
if (*s == ':') s++;
name[ptr++] = ':';
name[ptr] = 0;
return s;
}



/*************************************************
*           Pick out a number from a string      *
*************************************************/

/* Arguments:
  n     points to an integer into which to put the number
  s     points to the first digit of the number

Returns:  a pointer to the character after the last digit
*/
/*XXX consider expanding to int_eximarith_t.  But the test for
"overbig numbers" in 0002 still needs to overflow it. */

static uschar *
read_number(int *n, uschar *s)
{
*n = 0;
while (isdigit(*s)) *n = *n * 10 + (*s++ - '0');
return s;
}

static const uschar *
read_cnumber(int *n, const uschar *s)
{
*n = 0;
while (isdigit(*s)) *n = *n * 10 + (*s++ - '0');
return s;
}



/*************************************************
*        Extract keyed subfield from a string    *
*************************************************/

/* The yield is in dynamic store; NULL means that the key was not found.

Arguments:
  key       points to the name of the key
  s         points to the string from which to extract the subfield

Returns:    NULL if the subfield was not found, or
            a pointer to the subfield's data
*/

uschar *
expand_getkeyed(const uschar * key, const uschar * s)
{
int length = Ustrlen(key);
Uskip_whitespace(&s);

/* Loop to search for the key */

while (*s)
  {
  int dkeylength;
  uschar * data;
  const uschar * dkey = s;

  while (*s && *s != '=' && !isspace(*s)) s++;
  dkeylength = s - dkey;
  if (Uskip_whitespace(&s) == '=')
    while (isspace(*++s)) ;

  data = string_dequote(&s);
  if (length == dkeylength && strncmpic(key, dkey, length) == 0)
    return data;

  Uskip_whitespace(&s);
  }

return NULL;
}



static var_entry *
find_var_ent(const uschar * name, var_entry * table, unsigned nent)
{
int first = 0;
int last = nent;

while (last > first)
  {
  int middle = (first + last)/2;
  int c = Ustrcmp(name, table[middle].name);

  if (c > 0) { first = middle + 1; continue; }
  if (c < 0) { last = middle; continue; }
  return &table[middle];
  }
return NULL;
}

/*************************************************
*   Extract numbered subfield from string        *
*************************************************/

/* Extracts a numbered field from a string that is divided by tokens - for
example a line from /etc/passwd is divided by colon characters.  First field is
numbered one.  Negative arguments count from the right. Zero returns the whole
string. Returns NULL if there are insufficient tokens in the string

***WARNING***
Modifies final argument - this is a dynamically generated string, so that's OK.

Arguments:
  field       number of field to be extracted,
                first field = 1, whole string = 0, last field = -1
  separators  characters that are used to break string into tokens
  s           points to the string from which to extract the subfield

Returns:      NULL if the field was not found,
              a pointer to the field's data inside s (modified to add 0)
*/

static uschar *
expand_gettokened (int field, const uschar * separators, uschar * s)
{
int sep = 1;
int count;
uschar *ss = s;
uschar *fieldtext = NULL;

if (field == 0) return s;

/* Break the line up into fields in place; for field > 0 we stop when we have
done the number of fields we want. For field < 0 we continue till the end of
the string, counting the number of fields. */

count = (field > 0)? field : INT_MAX;

while (count-- > 0)
  {
  size_t len;

  /* Previous field was the last one in the string. For a positive field
  number, this means there are not enough fields. For a negative field number,
  check that there are enough, and scan back to find the one that is wanted. */

  if (sep == 0)
    {
    if (field > 0 || (-field) > (INT_MAX - count - 1)) return NULL;
    if ((-field) == (INT_MAX - count - 1)) return s;
    while (field++ < 0)
      {
      ss--;
      while (ss[-1] != 0) ss--;
      }
    fieldtext = ss;
    break;
    }

  /* Previous field was not last in the string; save its start and put a
  zero at its end. */

  fieldtext = ss;
  len = Ustrcspn(ss, separators);
  sep = ss[len];
  ss[len] = 0;
  ss += len + 1;
  }

return fieldtext;
}


static uschar *
expand_getlistele(int field, const uschar * list, int sep)
{
const uschar * tlist = list;
int sep_l = sep;
/* Tainted mem for the throwaway element copies */
uschar * dummy = store_get(2, GET_TAINTED);

if (field < 0)
  for (field++; string_nextinlist(&tlist, &sep_l, dummy, 1); ) field++;
if (field == 0) return NULL;
while (--field > 0 && (string_nextinlist(&list, &sep, dummy, 1))) ;
return string_nextinlist(&list, &sep, NULL, 0);
}


/* Certificate fields, by name.  Worry about by-OID later */
/* Names are chosen to not have common prefixes */

#ifndef DISABLE_TLS
typedef struct
{
uschar * name;
int      namelen;
uschar * (*getfn)(void * cert, const uschar * mod);
} certfield;
static certfield certfields[] =
{			/* linear search; no special order */
  { US"version",	 7,  &tls_cert_version },
  { US"serial_number",	 13, &tls_cert_serial_number },
  { US"subject",	 7,  &tls_cert_subject },
  { US"notbefore",	 9,  &tls_cert_not_before },
  { US"notafter",	 8,  &tls_cert_not_after },
  { US"issuer",		 6,  &tls_cert_issuer },
  { US"signature",	 9,  &tls_cert_signature },
  { US"sig_algorithm",	 13, &tls_cert_signature_algorithm },
  { US"subj_altname",    12, &tls_cert_subject_altname },
  { US"ocsp_uri",	 8,  &tls_cert_ocsp_uri },
  { US"crl_uri",	 7,  &tls_cert_crl_uri },
};

static uschar *
expand_getcertele(uschar * field, uschar * certvar)
{
var_entry * vp;

if (!(vp = find_var_ent(certvar, var_table, nelem(var_table))))
  {
  expand_string_message =
    string_sprintf("no variable named %q", certvar);
  return NULL;          /* Unknown variable name */
  }
/* NB this stops us passing certs around in variable.  Might
want to do that in future */
if (vp->type != vtype_cert)
  {
  expand_string_message =
    string_sprintf("%q is not a certificate", certvar);
  return NULL;          /* Unknown variable name */
  }
if (!*(void **)vp->value)
  return NULL;

if (*field >= '0' && *field <= '9')
  return tls_cert_ext_by_oid(*(void **)vp->value, field, 0);

for (certfield * cp = certfields;
     cp < certfields + nelem(certfields);
     cp++)
  if (Ustrncmp(cp->name, field, cp->namelen) == 0)
    {
    uschar * modifier = *(field += cp->namelen) == ','
      ? ++field : NULL;
    return (*cp->getfn)( *(void **)vp->value, modifier );
    }

expand_string_message =
  string_sprintf("bad field selector %q for certextract", field);
return NULL;
}
#endif	/*DISABLE_TLS*/

/*************************************************
*        Extract a substring from a string       *
*************************************************/

/* Perform the ${substr or ${length expansion operations.

Arguments:
  subject     the input string
  value1      the offset from the start of the input string to the start of
                the output string; if negative, count from the right.
  value2      the length of the output string, or negative (-1) for unset
                if value1 is positive, unset means "all after"
                if value1 is negative, unset means "all before"
  len         set to the length of the returned string

Returns:      pointer to the output string, or NULL if there is an error
*/

static uschar *
extract_substr(uschar *subject, int value1, int value2, int *len)
{
int sublen = Ustrlen(subject);

if (value1 < 0)    /* count from right */
  {
  value1 += sublen;

  /* If the position is before the start, skip to the start, and adjust the
  length. If the length ends up negative, the substring is null because nothing
  can precede. This falls out naturally when the length is unset, meaning "all
  to the left". */

  if (value1 < 0)
    {
    value2 += value1;
    if (value2 < 0) value2 = 0;
    value1 = 0;
    }

  /* Otherwise an unset length => characters before value1 */

  else if (value2 < 0)
    {
    value2 = value1;
    value1 = 0;
    }
  }

/* For a non-negative offset, if the starting position is past the end of the
string, the result will be the null string. Otherwise, an unset length means
"rest"; just set it to the maximum - it will be cut down below if necessary. */

else
  {
  if (value1 > sublen)
    {
    value1 = sublen;
    value2 = 0;
    }
  else if (value2 < 0) value2 = sublen;
  }

/* Cut the length down to the maximum possible for the offset value, and get
the required characters. */

if (value1 + value2 > sublen) value2 = sublen - value1;
*len = value2;
return subject + value1;
}




/*************************************************
*            Old-style hash of a string          *
*************************************************/

/* Perform the ${hash expansion operation.

Arguments:
  subject     the input string (an expanded substring)
  value1      the length of the output string; if greater or equal to the
                length of the input string, the input string is returned
  value2      the number of hash characters to use, or 26 if negative
  len         set to the length of the returned string

Returns:      pointer to the output string, or NULL if there is an error
*/

static uschar *
compute_hash(uschar * subject, int value1, int value2, int * len)
{
int sublen = Ustrlen(subject);

if (value2 <= 0)
  value2 = 26;
else if (value2 > Ustrlen(hashcodes))
  {
  expand_string_message =
    string_sprintf("hash count \"%d\" too big", value2);
  return NULL;
  }

/* Calculate the hash text. We know it is shorter than the original string, so
can safely place it in subject[] (we know that subject is always itself an
expanded substring). */

if (value1 < sublen)
  {
  int c;
  int i = 0;
  int j = value1;
  while ((c = (subject[j])) != 0)
    {
    int shift = (c + j++) & 7;
    subject[i] ^= (c << shift) | (c >> (8-shift));
    if (++i >= value1) i = 0;
    }
  for (i = 0; i < value1; i++)
    subject[i] = hashcodes[(subject[i]) % value2];
  }
else value1 = sublen;

*len = value1;
return subject;
}




/*************************************************
*             Numeric hash of a string           *
*************************************************/

/* Perform the ${nhash expansion operation. The first characters of the
string are treated as most important, and get the highest prime numbers.

Arguments:
  subject     the input string
  value1      the maximum value of the first part of the result
  value2      the maximum value of the second part of the result,
                or negative to produce only a one-part result
  len         set to the length of the returned string

Returns:  pointer to the output string, or NULL if there is an error.
*/

static uschar *
compute_nhash (uschar *subject, int value1, int value2, int *len)
{
uschar *s = subject;
int i = 0;
unsigned long int total = 0; /* no overflow */

while (*s != 0)
  {
  if (i == 0) i = nelem(prime) - 1;
  total += prime[i--] * (unsigned int)(*s++);
  }

/* If value2 is unset, just compute one number */

if (value2 < 0)
  s = string_sprintf("%lu", total % value1);

/* Otherwise do a div/mod hash */

else if (value1 == 0 || value2 == 0)
  return NULL;
else
  {
  total = total % (value1 * value2);
  s = string_sprintf("%lu/%lu", total/value2, total % value2);
  }

*len = Ustrlen(s);
return s;
}





/*************************************************
*     Find the value of a header or headers      *
*************************************************/

/* Multiple instances of the same header get concatenated, and this function
can also return a concatenation of all the header lines. When concatenating
specific headers that contain lists of addresses, a comma is inserted between
them. Otherwise we use a straight concatenation. Because some messages can have
pathologically large number of lines, there is a limit on the length that is
returned.

Arguments:
  name          the name of the header, without the leading $header_ or $h_,
                or NULL if a concatenation of all headers is required
  newsize       return the size of memory block that was obtained; may be NULL
                if exists_only is TRUE
  flags		FH_EXISTS_ONLY
		  set if called from a def: test; don't need to build a string;
		  just return a string that is not "" and not "0" if the header
		  exists
		FH_WANT_RAW
		  set if called for $rh_ or $rheader_ items; no processing,
		  other than concatenating, will be done on the header. Also used
		  for $message_headers_raw.
		FH_WANT_LIST
		  Double colon chars in the content, and replace newline with
		  colon between each element when concatenating; returning a
		  colon-sep list (elements might contain newlines)
  charset       name of charset to translate MIME words to; used only if
                want_raw is false; if NULL, no translation is done (this is
                used for $bh_ and $bheader_)

Returns:        NULL if the header does not exist, else a pointer to a new
                store block
*/

static uschar *
find_header(const uschar * name, int * newsize, unsigned flags,
  const uschar * charset)
{
BOOL found = !name;
int len = name ? Ustrlen(name) : 0;
BOOL comma = FALSE;
gstring * g = NULL;
uschar * rawhdr;

for (header_line * h = header_list; h; h = h->next)
  if (h->type != htype_old && h->text)  /* NULL => Received: placeholder */
    if (!name || (len <= h->slen && strncmpic(name, h->text, len) == 0))
      {
      uschar * s, * t;
      size_t inc;

      if (flags & FH_EXISTS_ONLY)
	return US"1";  /* don't need actual string */

      found = TRUE;
      s = h->text + len;		/* text to insert */
      if (!(flags & FH_WANT_RAW))	/* unless wanted raw, */
	Uskip_whitespace(&s);		/* remove leading white space */
      t = h->text + h->slen;		/* end-point */

      /* Unless wanted raw, remove trailing whitespace, including the
      newline. */

      if (flags & FH_WANT_LIST)
	while (t > s && t[-1] == '\n') t--;
      else if (!(flags & FH_WANT_RAW))
	{
	while (t > s && isspace(t[-1])) t--;

	/* Set comma if handling a single header and it's one of those
	that contains an address list, except when asked for raw headers. Only
	need to do this once. */

	if (name && !comma && Ustrchr("BCFRST", h->type)) comma = TRUE;
	}

      /* Trim the header roughly if we're approaching limits */
      inc = t - s;
      if (gstring_length(g) + inc > header_insert_maxlen)
	inc = header_insert_maxlen - gstring_length(g);

      /* For raw just copy the data; for a list, add the data as a colon-sep
      list-element; for comma-list add as an unchecked comma,newline sep
      list-elemment; for other nonraw add as an unchecked newline-sep list (we
      stripped trailing WS above including the newline). We ignore the potential
      expansion due to colon-doubling, just leaving the loop if the limit is met
      or exceeded. */

      if (flags & FH_WANT_LIST)
        g = string_append_listele_n(g, ':', s, (unsigned)inc);
      else if (flags & FH_WANT_RAW)
	g = string_catn(g, s, (unsigned)inc);
      else if (inc > 0)
	g = string_append2_listele_n(g, comma ? US",\n" : US"\n",
	  s, (unsigned)inc);

      if (gstring_length(g) >= header_insert_maxlen) break;
      }

if (!found) return NULL;	/* No header found */
if (!g) return US"";

/* That's all we do for raw header expansion. */

*newsize = g->size;
rawhdr = string_from_gstring(g);
if (flags & FH_WANT_RAW)
  return rawhdr;

/* Otherwise do RFC 2047 decoding, translating the charset if requested.
The rfc2047_decode2() function can return an error with decoded data if the
charset translation fails. If decoding fails, it returns NULL. */

else
  {
  uschar * error, * decoded = rfc2047_decode2(rawhdr,
    check_rfc2047_length, charset, '?', NULL, newsize, &error);
  if (error)
    DEBUG(D_any) debug_printf("*** error in RFC 2047 decoding: %s\n"
      "    input was: %s\n", error, rawhdr);
  return decoded ? decoded : rawhdr;
  }
}




/* Append a "local" element to an Authentication-Results: header
if this was a non-smtp message.
*/

static gstring *
authres_local(gstring * g, const uschar * sysname)
{
if (!f.authentication_local)
  return g;
g = string_append(g, 3, US";\n\tlocal=pass (non-smtp, ", sysname, US")");
if (authenticated_id) g = string_append(g, 2, " u=", authenticated_id);
return g;
}


/* Append an "iprev" element to an Authentication-Results: header
if we have attempted to get the calling host's name.
*/

static gstring *
authres_iprev(gstring * g)
{
if (sender_host_name)
  g = string_append(g, 3, US";\n\tiprev=pass (", sender_host_name, US")");
else if (host_lookup_deferred)
  g = string_cat(g, US";\n\tiprev=temperror");
else if (host_lookup_failed)
  g = string_cat(g, US";\n\tiprev=fail");
else
  return g;

if (sender_host_address)
  g = string_append(g, 2, US" smtp.remote-ip=", sender_host_address);
return g;
}



/*************************************************
*               Return list of recipients        *
*************************************************/
/* A recipients list is available only during system message filtering,
during ACL processing after DATA, and while expanding pipe commands
generated from a system filter, but not elsewhere.  Note that this does
not check for commas in the elements, and uses comma-space as seperator -
so cannot be used as an exim list as-is. */

static uschar *
fn_recipients(void)
{
gstring * g = NULL;

if (!f.enable_dollar_recipients) return NULL;

for (int i = 0; i < recipients_count; i++)
  {
  const uschar * s = recipients_list[i].address;
  g = string_append2_listele_n(g, US", ", s, Ustrlen(s));
  }
gstring_release_unused(g);
return string_from_gstring(g);
}

/* Similar, but as a properly-quoted exim list */


static uschar *
fn_recipients_list(void)
{
gstring * g = NULL;

if (!f.enable_dollar_recipients) return NULL;

for (int i = 0; i < recipients_count; i++)
  g = string_append_listele(g, ':', recipients_list[i].address);
gstring_release_unused(g);
return string_from_gstring(g);
}


/*************************************************
*               Return size of queue             *
*************************************************/
/* Ask the daemon for the queue size */

static uschar *
fn_queue_size(void)
{
struct sockaddr_un sa_un = {.sun_family = AF_UNIX};
uschar buf[16];
int fd;
ssize_t len;
const uschar * where;
uschar * sname;

if ((fd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
  {
  DEBUG(D_expand) debug_printf(" socket: %s\n", strerror(errno));
  return NULL;
  }

len = daemon_client_sockname(&sa_un, &sname);

if (bind(fd, (const struct sockaddr *)&sa_un, (socklen_t)len) < 0)
  { where = US"bind"; goto bad; }

#ifdef notdef
debug_printf("local addr '%s%s'\n",
  *sa_un.sun_path ? "" : "@",
  sa_un.sun_path + (*sa_un.sun_path ? 0 : 1));
#endif

len = daemon_notifier_sockname(&sa_un);
if (connect(fd, (const struct sockaddr *)&sa_un, len) < 0)
  { where = US"connect"; goto bad2; }

buf[0] = NOTIFY_QUEUE_SIZE_REQ;
if (send(fd, buf, 1, 0) < 0) { where = US"send"; goto bad; }

if (poll_one_fd(fd, POLLIN, 2 * 1000) != 1)
  {
  DEBUG(D_expand) debug_printf("no daemon response; using local evaluation\n");
  len = snprintf(CS buf, sizeof(buf), "%u", queue_count_cached());
  }
else if ((len = recv(fd, buf, sizeof(buf), 0)) < 0)
  { where = US"recv"; goto bad2; }

close(fd);
#ifndef EXIM_HAVE_ABSTRACT_UNIX_SOCKETS
Uunlink(sname);
#endif
return string_copyn(buf, len);

bad2:
#ifndef EXIM_HAVE_ABSTRACT_UNIX_SOCKETS
  Uunlink(sname);
#endif
bad:
  close(fd);
  DEBUG(D_expand) debug_printf(" %s: %s\n", where, strerror(errno));
  return NULL;
}


/*************************************************
*               Find value of a variable         *
*************************************************/

/* The table of variables is kept in alphabetic order, so we can search it
using a binary chop. The "choplen" variable is nothing to do with the binary
chop.

Arguments:
  name          the name of the variable being sought
  flags
    exists_only  TRUE if this is a def: test; passed on to find_header()
    skipping     TRUE => skip any processing evaluation; this is not the same as
                  exists_only because def: may test for values that are first
                  evaluated here
  newsize       pointer to an int which is initially zero; if the answer is in
                a new memory buffer, *newsize is set to its size

Returns:        NULL if the variable does not exist, or
                a pointer to the variable's contents, or
                something non-NULL if exists_only is TRUE
*/

static const uschar *
find_variable(uschar * name, esi_flags flags, int * newsize)
{
var_entry * vp;
uschar * s, * domain;
uschar ** ss;
void * val;
var_entry * table = var_table;
unsigned table_count = nelem(var_table);

/* Handle ACL variables, whose names are of the form acl_cxxx or acl_mxxx.
Originally, xxx had to be a number in the range 0-9 (later 0-19), but from
release 4.64 onwards arbitrary names are permitted, as long as the first 5
characters are acl_c or acl_m and the sixth is either a digit or an underscore
(this gave backwards compatibility at the changeover). There may be built-in
variables whose names start acl_ but they should never start in this way. This
slightly messy specification is a consequence of the history, needless to say.

If an ACL variable does not exist, treat it as empty, unless strict_acl_vars is
set, in which case give an error. */

if ((Ustrncmp(name, "acl_c", 5) == 0 || Ustrncmp(name, "acl_m", 5) == 0) &&
     !isalpha(name[5]))
  {
  tree_node * node =
    tree_search(name[4] == 'c' ? acl_var_c : acl_var_m, name + 4);
  return node ? node->data.ptr : strict_acl_vars ? NULL : US"";
  }
else if (Ustrncmp(name, "r_", 2) == 0)
  {
  tree_node * node = tree_search(router_var, name + 2);
  return node ? node->data.ptr : strict_acl_vars ? NULL : US"";
  }

/* Handle $auth<n>, $regex<n> variables. */

if (Ustrncmp(name, "auth", 4) == 0)
  {
  uschar *endptr;
  int n = Ustrtoul(name + 4, &endptr, 10);
  if (!*endptr && n != 0 && n <= AUTH_VARS)
    return auth_vars[n-1] ? auth_vars[n-1] : US"";
  }
#ifdef WITH_CONTENT_SCAN
else if (Ustrncmp(name, "regex", 5) == 0)
  {
  uschar *endptr;
  int n = Ustrtoul(name + 5, &endptr, 10);
  if (!*endptr && n != 0 && n <= REGEX_VARS)
    return regex_vars[n-1] ? regex_vars[n-1] : US"";
  }
#endif

sublist:

/* For all other variables, search the table */

if (!(vp = find_var_ent(name, table, table_count)))
  return NULL;          /* Unknown variable name */

/* Found an existing variable. If in skipping state, the value isn't needed,
and we want to avoid processing (such as looking up the host name). */

if (flags & ESI_SKIPPING)
  return US"";

val = vp->value;
switch (vp->type)
  {
  case vtype_filter_int:
    if (!f.filter_running) return NULL;
    /* Fall through */
    /* VVVVVVVVVVVV */
  case vtype_int:
    sprintf(CS var_buffer, "%d", *(int *)(val)); /* Integer */
    return var_buffer;

  case vtype_ino:
    sprintf(CS var_buffer, "%ld", (long int)(*(ino_t *)(val))); /* Inode */
    return var_buffer;

  case vtype_gid:
    sprintf(CS var_buffer, "%ld", (long int)(*(gid_t *)(val))); /* gid */
    return var_buffer;

  case vtype_uid:
    sprintf(CS var_buffer, "%ld", (long int)(*(uid_t *)(val))); /* uid */
    return var_buffer;

  case vtype_bool:
    sprintf(CS var_buffer, "%s", *(BOOL *)(val) ? "yes" : "no"); /* bool */
    return var_buffer;

  case vtype_stringptr:                      /* Pointer to string */
    return (s = *((uschar **)(val))) ? s : US"";

  case vtype_pid:
    sprintf(CS var_buffer, "%d", (int)getpid()); /* pid */
    return var_buffer;

  case vtype_load_avg:
    sprintf(CS var_buffer, "%d", OS_GETLOADAVG()); /* load_average */
    return var_buffer;

  case vtype_host_lookup:                    /* Lookup if not done so */
    if (  !sender_host_name && sender_host_address
       && !host_lookup_failed && host_name_lookup() == OK)
      host_build_sender_fullhost();
    return sender_host_name ? sender_host_name : US"";

  case vtype_localpart:                      /* Get local part from address */
    if (!(s = *((uschar **)(val)))) return US"";
    if (!(domain = Ustrrchr(s, '@'))) return s;
    if (domain - s > sizeof(var_buffer) - 1)
      log_write_die(0, LOG_MAIN, "local part longer than " SIZE_T_FMT
	  " in string expansion", sizeof(var_buffer));
    return string_copyn(s, domain - s);

  case vtype_domain:                         /* Get domain from address */
    if (!(s = *((uschar **)(val)))) return US"";
    domain = Ustrrchr(s, '@');
    return domain ? domain + 1 : US"";

  case vtype_msgheaders:
    return find_header(NULL, newsize,
	    flags & ESI_EXISTS_ONLY ? FH_EXISTS_ONLY : 0, NULL);

  case vtype_msgheaders_raw:
    return find_header(NULL, newsize,
	    flags & ESI_EXISTS_ONLY ? FH_EXISTS_ONLY|FH_WANT_RAW : FH_WANT_RAW,
	    NULL);

  case vtype_msgbody:                        /* Pointer to msgbody string */
  case vtype_msgbody_end:                    /* Ditto, the end of the msg */
    ss = (uschar **)(val);
    if (!*ss && deliver_datafile >= 0)  /* Read body when needed */
      {
      uschar * body;
      off_t start_offset_o = spool_data_start_offset(message_id);
      off_t start_offset = start_offset_o;
      int len = message_body_visible;

      if (len > message_size) len = message_size;
      *ss = body = store_get(len+1, GET_TAINTED);
      body[0] = 0;
      if (vp->type == vtype_msgbody_end)
	{
	struct stat statbuf;
	if (fstat(deliver_datafile, &statbuf) == 0)
	  {
	  start_offset = statbuf.st_size - len;
	  if (start_offset < start_offset_o)
	    start_offset = start_offset_o;
	  }
	}
      if (lseek(deliver_datafile, start_offset, SEEK_SET) < 0)
	log_write_die(0, LOG_MAIN, "deliver_datafile lseek: %s",
	  strerror(errno));
      if ((len = read(deliver_datafile, body, len)) > 0)
	{
	body[len] = 0;
	if (message_body_newlines)   /* Separate loops for efficiency */
	  while (len > 0)
	    { if (body[--len] == 0) body[len] = ' '; }
	else
	  while (len > 0)
	    { if (body[--len] == '\n' || body[len] == 0) body[len] = ' '; }
	}
      }
    return *ss ? *ss : US"";

  case vtype_todbsdin:                       /* BSD inbox time of day */
    return tod_stamp(tod_bsdin);

  case vtype_tode:                           /* Unix epoch time of day */
    return tod_stamp(tod_epoch);

  case vtype_todel:                          /* Unix epoch/usec time of day */
    return tod_stamp(tod_epoch_l);

  case vtype_todf:                           /* Full time of day */
    return tod_stamp(tod_full);

  case vtype_todl:                           /* Log format time of day */
    return tod_stamp(tod_log_bare);            /* (without timezone) */

  case vtype_todzone:                        /* Time zone offset only */
    return tod_stamp(tod_zone);

  case vtype_todzulu:                        /* Zulu time */
    return tod_stamp(tod_zulu);

  case vtype_todlf:                          /* Log file datestamp tod */
    return tod_stamp(tod_log_datestamp_daily);

  case vtype_reply:                          /* Get reply address */
    s = find_header(US"reply-to:", newsize,
	    flags & ESI_EXISTS_ONLY ? FH_EXISTS_ONLY|FH_WANT_RAW : FH_WANT_RAW,
	    headers_charset);
    if (s) Uskip_whitespace(&s);
    if (!s || !*s)
      {
      if (newsize) *newsize = 0;		/* For the *s==0 case */
      s = find_header(US"from:", newsize,
	    flags & ESI_EXISTS_ONLY ? FH_EXISTS_ONLY|FH_WANT_RAW : FH_WANT_RAW,
	    headers_charset);
      }
    if (s)
      {
      uschar *t;
      Uskip_whitespace(&s);
      for (t = s; *t; t++) if (*t == '\n') *t = ' ';
      while (t > s && isspace(t[-1])) t--;
      *t = 0;
      }
    return s ? s : US"";

  case vtype_string_func:
    {
    stringptr_fn_t * fn = (stringptr_fn_t *) val;
    uschar * s = fn();
    return s ? s : US"";
    }

  case vtype_pspace:
    {
    int inodes;
    sprintf(CS var_buffer, PR_EXIM_ARITH,
      receive_statvfs(val == (void *)TRUE, &inodes));
    }
  return var_buffer;

  case vtype_pinodes:
    {
    int inodes;
    (void) receive_statvfs(val == (void *)TRUE, &inodes);
    sprintf(CS var_buffer, "%d", inodes);
    }
  return var_buffer;

  case vtype_cert:
    return *(void **)val ? US"<cert>" : US"";

#ifndef DISABLE_DKIM
  case vtype_dkim:
    {
    misc_module_info * mi = misc_mod_findonly(US"dkim");
    typedef uschar * (*fn_t)(int);
    return mi
      ? (((fn_t *) mi->functions)[DKIM_EXPAND_QUERY]) ((int)(long)val)
      : US"";
    }
#endif

  case vtype_module:
    {
    uschar * errstr;
    misc_module_info * mi = misc_mod_find(val, &errstr);
    if (mi)
      {
      table = mi->variables;
      table_count = mi->variables_count;
      goto sublist;
      }
    log_write(0, LOG_MAIN|LOG_PANIC,
      "failed to find %s module for %s: %s", US val, name, errstr);
    return US"";
    }
  }

return NULL;  /* Unknown variable. Silences static checkers. */
}




void
modify_variable(uschar *name, void * value)
{
var_entry * vp;
if ((vp = find_var_ent(name, var_table, nelem(var_table))))
  vp->value = value;
return;          /* Unknown variable name, fail silently */
}






/*************************************************
*           Read and expand substrings           *
*************************************************/

/* This function is called to read and expand argument substrings for various
expansion items. Some have a minimum requirement that is less than the maximum;
in these cases, the first non-present one is set to NULL.

Arguments:
  sub        points to vector of pointers to set
  n          maximum number of substrings
  m          minimum required
  sptr       points to current string pointer
  flags
   skipping   the skipping flag
  check_end  if TRUE, check for final '}'
  name       name of item, for error message
  resetok    if not NULL, pointer to flag - write FALSE if unsafe to reset
	     the store
  textonly_p if not NULL, pointer to bitmask of which subs were text-only
	     (did not change when expended)

Returns:     -1 OK; string pointer updated, but in "skipping" mode
	     0 OK; string pointer updated
             1 curly bracketing error (too few arguments)
             2 too many arguments (only if check_end is set); message set
             3 other error (expansion failure)
*/

static int
read_subs(uschar ** sub, int n, int m, const uschar ** sptr, esi_flags flags,
  BOOL check_end, uschar * name, BOOL * resetok, unsigned * textonly_p)
{
const uschar * s = *sptr;
unsigned textonly_l = 0;

Uskip_whitespace(&s);
for (int i = 0; i < n; i++)
  {
  BOOL textonly;
  if (*s != '{')
    {
    if (i < m)
      {
      expand_string_message = string_sprintf("Not enough arguments for '%s' "
	"(min is %d)", name, m);
      return 1;
      }
    sub[i] = NULL;
    break;
    }
  if (!(sub[i] = expand_string_internal(s+1,
	  ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags & ESI_SKIPPING, &s, resetok,
	  textonly_p ? &textonly : NULL)))
    return 3;
  if (*s++ != '}') return 1;
  if (textonly_p && textonly) textonly_l |= BIT(i);
  Uskip_whitespace(&s);
  }						/*{*/
if (check_end && *s++ != '}')
  {
  if (s[-1] == '{')
    {
    expand_string_message = string_sprintf("Too many arguments for '%s' "
      "(max is %d)", name, n);
    return 2;
    }
  expand_string_message = string_sprintf("missing '}' after '%s'", name);
  return 1;
  }

if (textonly_p) *textonly_p = textonly_l;
*sptr = s;
return flags & ESI_SKIPPING ? -1 : 0;
}




/*************************************************
*     Elaborate message for bad variable         *
*************************************************/

/* For the "unknown variable" message, take a look at the variable's name, and
give additional information about possible ACL variables. The extra information
is added on to expand_string_message.

Argument:   the name of the variable
Returns:    nothing
*/

static void
check_variable_error_message(uschar *name)
{
if (Ustrncmp(name, "acl_", 4) == 0)
  expand_string_message = string_sprintf("%s (%s)", expand_string_message,
    (name[4] == 'c' || name[4] == 'm')?
      (isalpha(name[5])?
        US"6th character of a user-defined ACL variable must be a digit or underscore" :
        US"strict_acl_vars is set"    /* Syntax is OK, it has to be this */
      ) :
      US"user-defined ACL variables must start acl_c or acl_m");
}



/*
Load args from sub array to globals, and call acl_check().
Sub array will be corrupted on return.

Returns:       OK         access is granted by an ACCEPT verb
               DISCARD    access is (apparently) granted by a DISCARD verb
	       FAIL       access is denied
	       FAIL_DROP  access is denied; drop the connection
	       DEFER      can't tell at the moment
	       ERROR      disaster
*/
static int
eval_acl(uschar ** sub, int nsub, uschar ** user_msgp)
{
int i;
int sav_narg = acl_narg;
int ret;
uschar * dummy_logmsg;

if(--nsub > nelem(acl_arg)) nsub = nelem(acl_arg);
for (i = 0; i < nsub && sub[i+1]; i++)
  {
  uschar * tmp = acl_arg[i];
  acl_arg[i] = sub[i+1];	/* place callers args in the globals */
  sub[i+1] = tmp;		/* stash the old args using our caller's storage */
  }
acl_narg = i;
while (i < nsub)
  {
  sub[i+1] = acl_arg[i];
  acl_arg[i++] = NULL;
  }

DEBUG(D_expand)
  debug_printf_indent("expanding: acl: %s  arg: %s%s\n",
    sub[0],
    acl_narg>0 ? acl_arg[0] : US"<none>",
    acl_narg>1 ? " +more"   : "");

ret = acl_eval(acl_where, sub[0], user_msgp, &dummy_logmsg);

for (i = 0; i < nsub; i++)
  acl_arg[i] = sub[i+1];	/* restore old args */
acl_narg = sav_narg;

return ret;
}




/* Return pointer to dewrapped string, with enclosing specified chars removed.
The given string is modified on return.  Leading whitespace is skipped while
looking for the opening wrap character, then the rest is scanned for the trailing
(non-escaped) wrap character.  A backslash in the string will act as an escape.

A nul is written over the trailing wrap, and a pointer to the char after the
leading wrap is returned.

Arguments:
  s	String for de-wrapping
  wrap  Two-char string, the first being the opener, second the closer wrapping
        character
Return:
  Pointer to de-wrapped string, or NULL on error (with expand_string_message set).
*/

static uschar *
dewrap(uschar * s, const uschar * wrap)
{
uschar * p = s;
unsigned depth = 0;
BOOL quotesmode = wrap[0] == wrap[1];

if (Uskip_whitespace(&p) == *wrap)
  {
  s = ++p;
  wrap++;
  while (*p)
    {
    if (*p == '\\') p++;
    else if (!quotesmode && *p == wrap[-1]) depth++;
    else if (*p == *wrap)
      if (depth == 0)
	{
	*p = '\0';
	return s;
	}
      else
	depth--;
    p++;
    }
  }
expand_string_message = string_sprintf("missing '%c'", *wrap);
return NULL;
}


/* Pull off the leading array or object element, returning
a copy in an allocated string.  Update the list pointer.

The element may itself be an abject or array.
Return NULL when the list is empty.
*/

static uschar *
json_nextinlist(const uschar ** list)
{
unsigned array_depth = 0, object_depth = 0;
BOOL quoted = FALSE;
const uschar * s = *list, * item;

skip_whitespace(&s);

for (item = s;
     *s && (*s != ',' || array_depth != 0 || object_depth != 0 || quoted);
     s++)
  if (!quoted) switch (*s)
    {
    case '[': array_depth++; break;
    case ']': array_depth--; break;
    case '{': object_depth++; break;
    case '}': object_depth--; break;
    case '"': quoted = TRUE;
    }
  else switch(*s)
    {
    case '\\': s++; break;		/* backslash protects one char */
    case '"':  quoted = FALSE; break;
    }
*list = *s ? s+1 : s;
if (item == s) return NULL;
item = string_copyn(item, s - item);
DEBUG(D_expand) debug_printf_indent("  json ele: '%s'\n", item);
return US item;
}



/************************************************/
/*  Return offset in ops table, or -1 if not found.
Repoint to just after the operator in the string.

Argument:
 ss	string representation of operator
 opname	split-out operator name
*/

static int
identify_operator(const uschar ** ss, uschar ** opname)
{
const uschar * s = *ss;
uschar name[256];

/* Numeric comparisons are symbolic */

if (*s == '=' || *s == '>' || *s == '<')
  {
  int p = 0;
  name[p++] = *s++;
  if (*s == '=')
    {
    name[p++] = '=';
    s++;
    }
  name[p] = 0;
  }

/* All other conditions are named */

else
  s = read_name(name, sizeof(name), s, US"_");
*ss = s;

/* If we haven't read a name, it means some non-alpha character is first. */

if (!name[0])
  {
  expand_string_message = string_sprintf("condition name expected, "
    "but found \"%.16s\"", s);
  return -1;
  }
DEBUG(D_expand) debug_printf_indent("cond: %s\n", name);
if (opname)
  *opname = string_copy(name);

return chop_match(name, cond_table, nelem(cond_table));
}


/*************************************************
*    Handle MD5 or SHA-1 computation for HMAC    *
*************************************************/

/* These are some wrapping functions that enable the HMAC code to be a bit
cleaner. A good compiler will spot the tail recursion.

Arguments:
  type         HMAC_MD5 or HMAC_SHA1
  remaining    are as for the cryptographic hash functions

Returns:       nothing
*/

static void
chash_start(int type, void * base)
{
if (type == HMAC_MD5)
  md5_start((md5 *)base);
else
  sha1_start((hctx *)base);
}

static void
chash_mid(int type, void * base, const uschar * string)
{
if (type == HMAC_MD5)
  md5_mid((md5 *)base, string);
else
  sha1_mid((hctx *)base, string);
}

static void
chash_end(int type, void * base, const uschar * string, int length,
  uschar * digest)
{
if (type == HMAC_MD5)
  md5_end((md5 *)base, string, length, digest);
else
  sha1_end((hctx *)base, string, length, digest);
}




#ifdef SUPPORT_SRS
/* Do an hmac_md5.  The result is _not_ nul-terminated, and is sized as
the smaller of a full hmac_md5 result (16 bytes) or the supplied output buffer.

Arguments:
	key	encoding key, nul-terminated
	src	data to be hashed, nul-terminated
	buf	output buffer
	len	size of output buffer
*/

static void
hmac_md5(const uschar * key, const uschar * src, uschar * buf, unsigned len)
{
md5 md5_base;
const uschar * keyptr;
uschar * p;
unsigned int keylen;

#define MD5_HASHLEN      16
#define MD5_HASHBLOCKLEN 64

uschar keyhash[MD5_HASHLEN];
uschar innerhash[MD5_HASHLEN];
uschar finalhash[MD5_HASHLEN];
uschar innerkey[MD5_HASHBLOCKLEN];
uschar outerkey[MD5_HASHBLOCKLEN];

keyptr = key;
keylen = Ustrlen(keyptr);

/* If the key is longer than the hash block length, then hash the key
first */

if (keylen > MD5_HASHBLOCKLEN)
  {
  chash_start(HMAC_MD5, &md5_base);
  chash_end(HMAC_MD5, &md5_base, keyptr, keylen, keyhash);
  keyptr = keyhash;
  keylen = MD5_HASHLEN;
  }

/* Now make the inner and outer key values */

memset(innerkey, 0x36, MD5_HASHBLOCKLEN);
memset(outerkey, 0x5c, MD5_HASHBLOCKLEN);

for (int i = 0; i < keylen; i++)
  {
  innerkey[i] ^= keyptr[i];
  outerkey[i] ^= keyptr[i];
  }

/* Now do the hashes */

chash_start(HMAC_MD5, &md5_base);
chash_mid(HMAC_MD5, &md5_base, innerkey);
chash_end(HMAC_MD5, &md5_base, src, Ustrlen(src), innerhash);

chash_start(HMAC_MD5, &md5_base);
chash_mid(HMAC_MD5, &md5_base, outerkey);
chash_end(HMAC_MD5, &md5_base, innerhash, MD5_HASHLEN, finalhash);

/* Encode the final hash as a hex string, limited by output buffer size */

p = buf;
for (int i = 0, j = len; i < MD5_HASHLEN; i++)
  {
  if (j-- <= 0) break;
  *p++ = hex_digits[(finalhash[i] & 0xf0) >> 4];
  if (j-- <= 0) break;
  *p++ = hex_digits[finalhash[i] & 0x0f];
  }
return;
}
#endif /*SUPPORT_SRS*/


/*************************************************
*        Read and evaluate a condition           *
*************************************************/

/*
Arguments:
  s        points to the start of the condition text
  resetok  points to a BOOL which is written false if it is unsafe to
	   free memory. Certain condition types (acl) may have side-effect
	   allocation which must be preserved.
  yield    points to a BOOL to hold the result of the condition test;
           if NULL, we are just reading through a condition that is
           part of an "or" combination to check syntax, or in a state
           where the answer isn't required

Returns:   a pointer to the first character after the condition, or
           NULL after an error
*/

static const uschar *
eval_condition(const uschar * s, BOOL * resetok, BOOL * yield)
{
BOOL testfor = TRUE, tempcond, combined_cond;
BOOL * subcondptr;
BOOL sub2_honour_dollar = TRUE, is_forany, is_json, is_jsons;
int rc, cond_type;
int_eximarith_t num[2];
struct stat statbuf;
uschar * opname;
uschar name[256];
const uschar * sub[10], * next;
unsigned sub_textonly = 0;

expand_level++;
for (;;)
  if (Uskip_whitespace(&s) == '!') { testfor = !testfor; s++; } else break;

switch(cond_type = identify_operator(&s, &opname))
  {
  /* def: tests for a non-empty variable, or for the existence of a header. If
  yield == NULL we are in a skipping state, and don't care about the answer. */

  case ECOND_DEF:
    {
    const uschar * t;

    if (*s != ':')
      {
      expand_string_message = US"\":\" expected after \"def\"";
      goto failout;
      }

    s = read_name(name, sizeof(name), s+1, US"_");

    /* Test for a header's existence. If the name contains a closing brace
    character, this may be a user error where the terminating colon has been
    omitted. Set a flag to adjust a subsequent error message in this case. */

    if (  ( *(t = name) == 'h'
	  || (*t == 'r' || *t == 'l' || *t == 'b') && *++t == 'h'
	  )
       && (*++t == '_' || Ustrncmp(t, "eader_", 6) == 0)
       )
      {
      s = read_header_name(name, sizeof(name), s);
      /* {-for-text-editors */
      if (Ustrchr(name, '}') != NULL) malformed_header = TRUE;
      if (yield) *yield =
	(find_header(name, NULL, FH_EXISTS_ONLY, NULL) != NULL) == testfor;
      }

    /* Test for a variable's having a non-empty value. A non-existent variable
    under strict_acl_vars causes an expansion failure. */

    else
      {
      if (!(t = find_variable(name,
	yield ? ESI_EXISTS_ONLY : ESI_EXISTS_ONLY | ESI_SKIPPING, NULL)))
	{
	expand_string_message = name[0]
	  ? string_sprintf("unknown variable %q after \"def:\"", name)
	  : US"variable name omitted after \"def:\"";
	check_variable_error_message(name);
	goto failout;
	}
      if (yield) *yield = (t[0] != 0) == testfor;
      }

    next = s; goto out;
    }


  /* first_delivery tests for first delivery attempt */

  case ECOND_FIRST_DELIVERY:
  if (yield) *yield = f.deliver_firsttime == testfor;
  next = s; goto out;


  /* queue_running tests for any process started by a queue runner */

  case ECOND_QUEUE_RUNNING:
  if (yield) *yield = (queue_run_pid != (pid_t)0) == testfor;
  next = s; goto out;


  /* exists:  tests for file existence
       isip:  tests for any IP address
      isip4:  tests for an IPv4 address
      isip6:  tests for an IPv6 address
        pam:  does PAM authentication
     radius:  does RADIUS authentication
   ldapauth:  does LDAP authentication
    pwcheck:  does Cyrus SASL pwcheck authentication
  */

  case ECOND_EXISTS:
  case ECOND_ISIP:
  case ECOND_ISIP4:
  case ECOND_ISIP6:
  case ECOND_PAM:
  case ECOND_RADIUS:
  case ECOND_LDAPAUTH:
  case ECOND_PWCHECK:

  if (Uskip_whitespace(&s) != '{') goto COND_FAILED_CURLY_START; /* }-for-text-editors */

   {
    BOOL textonly;
    sub[0] = expand_string_internal(s+1,
      ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | (yield ? ESI_NOFLAGS : ESI_SKIPPING),
      &s, resetok, &textonly);
    if (!sub[0]) goto failout;
    if (textonly) sub_textonly |= BIT(0);
   }
  /* {-for-text-editors */
  if (*s++ != '}') goto COND_FAILED_CURLY_END;

  if (!yield) { next = s; goto out; }  /* No need to run the test if skipping */

  switch(cond_type)
    {
    case ECOND_EXISTS:
    if ((expand_forbid & RDO_EXISTS) != 0)
      {
      expand_string_message = US"File existence tests are not permitted";
      goto failout;
      }
    *yield = (Ustat(sub[0], &statbuf) == 0) == testfor;
    break;

    case ECOND_ISIP:
    case ECOND_ISIP4:
    case ECOND_ISIP6:
    {
      const uschar *errp;
      const uschar **errpp;
      DEBUG(D_expand) errpp = &errp; else errpp = 0;
      if (0 == (rc = string_is_ip_addressX(sub[0], NULL, errpp)))
        DEBUG(D_expand) debug_printf("failed: %s\n", errp);

      *yield = ( cond_type == ECOND_ISIP  ? rc != 0 :
                 cond_type == ECOND_ISIP4 ? rc == 4 : rc == 6) == testfor;
    }

    break;

    /* Various authentication tests - all optionally compiled */

    case ECOND_PAM:
#ifdef SUPPORT_PAM
      {
      const misc_module_info * mi = misc_mod_find(US"pam", NULL);
      typedef int (*fn_t)(const uschar *, uschar **);
      if (!mi)
	goto COND_FAILED_NOT_COMPILED;
      rc = (((fn_t *) mi->functions)[PAM_AUTH_CALL])
					  (sub[0], &expand_string_message);
      goto END_AUTH;
      }
#else
      goto COND_FAILED_NOT_COMPILED;
#endif  /* SUPPORT_PAM */

    case ECOND_RADIUS:
#ifdef RADIUS_CONFIG_FILE
      {
      const misc_module_info * mi = misc_mod_find(US"radius", NULL);
      typedef int (*fn_t)(const uschar *, uschar **);
      if (!mi)
	goto COND_FAILED_NOT_COMPILED;
      rc = (((fn_t *) mi->functions)[RADIUS_AUTH_CALL])
					  (sub[0], &expand_string_message);
      goto END_AUTH;
      }
#else
      goto COND_FAILED_NOT_COMPILED;
#endif  /* RADIUS_CONFIG_FILE */

    case ECOND_LDAPAUTH:
    #ifdef LOOKUP_LDAP
      {
      int expand_setup = -1;
      const lookup_info * li = search_findtype(US"ldapauth", 8);
      void * handle;

      if (li && (handle = search_open(NULL, li, 0, NULL, NULL)))
	rc = search_find(handle, NULL, sub[0],
			-1, NULL, 0, 0, &expand_setup, NULL)
	  ? OK : f.search_find_defer ? DEFER : FAIL;
      else
	{ expand_string_message = search_error_message; rc = FAIL; }
      }
    goto END_AUTH;
    #else
    goto COND_FAILED_NOT_COMPILED;
    #endif  /* LOOKUP_LDAP */

    case ECOND_PWCHECK:
    #ifdef CYRUS_PWCHECK_SOCKET
    rc = auth_call_pwcheck(sub[0], &expand_string_message);
    goto END_AUTH;
    #else
    goto COND_FAILED_NOT_COMPILED;
    #endif  /* CYRUS_PWCHECK_SOCKET */

    #if defined(SUPPORT_PAM) || defined(RADIUS_CONFIG_FILE) || \
        defined(LOOKUP_LDAP) || defined(CYRUS_PWCHECK_SOCKET)
    END_AUTH:
    if (rc == ERROR || rc == DEFER) goto failout;
    *yield = (rc == OK) == testfor;
    #endif
    }
  next = s; goto out;


  /* call ACL (in a conditional context).  Accept true, deny false.
  Defer is a forced-fail.  Anything set by message= goes to $value.
  Up to ten parameters are used; we use the braces round the name+args
  like the saslauthd condition does, to permit a variable number of args.
  See also the expansion-item version EITEM_ACL and the traditional
  acl modifier ACLC_ACL.
  Since the ACL may allocate new global variables, tell our caller to not
  reclaim memory.
  */

  case ECOND_ACL:
    /* ${if acl {{name}{arg1}{arg2}...}  {yes}{no}} */
    {
    uschar * sub[10];
    uschar * user_msg;
    BOOL cond = FALSE;

    Uskip_whitespace(&s);
    if (*s++ != '{') goto COND_FAILED_CURLY_START;	/*}*/

    switch(read_subs(sub, nelem(sub), 1, &s,
	yield ? ESI_NOFLAGS : ESI_SKIPPING, TRUE, name, resetok, NULL))
      {
      case 1: expand_string_message = US"too few arguments or bracketing "
        "error for acl";
      case 2:
      case 3: goto failout;
      }

    if (yield)
      {
      int rc;
      *resetok = FALSE;	/* eval_acl() might allocate; do not reclaim */
      switch(rc = eval_acl(sub, nelem(sub), &user_msg))
	{
	case OK:
	  cond = TRUE;
	case FAIL:
          lookup_value = NULL;
	  if (user_msg)
            lookup_value = string_copy(user_msg);
	  *yield = cond == testfor;
	  break;

	case DEFER:
          f.expand_string_forcedfail = TRUE;
	  /*FALLTHROUGH*/
	default:
          expand_string_message = string_sprintf("%s from acl %q",
	    rc_names[rc], sub[0]);
	  goto failout;
	}
      }
    next = s; goto out;
    }


  /* saslauthd: does Cyrus saslauthd authentication. Four parameters are used:

     ${if saslauthd {{username}{password}{service}{realm}}  {yes}{no}}

  However, the last two are optional. That is why the whole set is enclosed
  in their own set of braces. */

  case ECOND_SASLAUTHD:
#ifndef CYRUS_SASLAUTHD_SOCKET
    goto COND_FAILED_NOT_COMPILED;
#else
    {
    uschar *sub[4];
    Uskip_whitespace(&s);
    if (*s++ != '{') goto COND_FAILED_CURLY_START;	/* }-for-text-editors */
    switch(read_subs(sub, nelem(sub), 2, &s,
	yield ? ESI_NOFLAGS : ESI_SKIPPING, TRUE, name, resetok, NULL))
      {
      case 1: expand_string_message = US"too few arguments or bracketing "
	"error for saslauthd";
      case 2:
      case 3: goto failout;
      }
    if (!sub[2]) sub[3] = NULL;  /* realm if no service */
    if (yield)
      {
      int rc = auth_call_saslauthd(sub[0], sub[1], sub[2], sub[3],
	&expand_string_message);
      if (rc == ERROR || rc == DEFER) goto failout;
      *yield = (rc == OK) == testfor;
      }
    next = s; goto out;
    }
#endif /* CYRUS_SASLAUTHD_SOCKET */


  /* symbolic operators for numeric and string comparison, and a number of
  other operators, all requiring two arguments.

  crypteq:           encrypts plaintext and compares against an encrypted text,
                       using crypt(), crypt16(), MD5 or SHA-1
  inlist/inlisti:    checks if first argument is in the list of the second
  match:             does a regular expression match and sets up the numerical
                       variables if it succeeds
  match_address:     matches in an address list
  match_domain:      matches in a domain list
  match_ip:          matches a host list that is restricted to IP addresses
  match_local_part:  matches in a local part list
  */

  case ECOND_MATCH_ADDRESS:
  case ECOND_MATCH_DOMAIN:
  case ECOND_MATCH_IP:
  case ECOND_MATCH_LOCAL_PART:
    sub2_honour_dollar = FALSE;
    /* FALLTHROUGH */

  case ECOND_CRYPTEQ:
  case ECOND_INLIST:
  case ECOND_INLISTI:
  case ECOND_MATCH:

  case ECOND_NUM_L:     /* Numerical comparisons */
  case ECOND_NUM_LE:
  case ECOND_NUM_E:
  case ECOND_NUM_EE:
  case ECOND_NUM_G:
  case ECOND_NUM_GE:

  case ECOND_STR_LT:    /* String comparisons */
  case ECOND_STR_LTI:
  case ECOND_STR_LE:
  case ECOND_STR_LEI:
  case ECOND_STR_EQ:
  case ECOND_STR_EQI:
  case ECOND_STR_GT:
  case ECOND_STR_GTI:
  case ECOND_STR_GE:
  case ECOND_STR_GEI:

  for (int i = 0; i < 2; i++)
    {
    BOOL textonly;
    /* Sometimes, we don't expand substrings; too many insecure configurations
    created using match_address{}{} and friends, where the second param
    includes information from untrustworthy sources. */
    /*XXX is this moot given taint-tracking? */

    esi_flags flags = ESI_BRACE_ENDS;

    if (!(i > 0 && !sub2_honour_dollar)) flags |= ESI_HONOR_DOLLAR;
    if (!yield) flags |= ESI_SKIPPING;

    if (Uskip_whitespace(&s) != '{')
      {
      if (i == 0) goto COND_FAILED_CURLY_START;
      expand_string_message = string_sprintf("missing 2nd string in {} "
        "after %q", opname);
      goto failout;
      }
    if (!(sub[i] = expand_string_internal(s+1, flags, &s, resetok, &textonly)))
      goto failout;
    if (textonly) sub_textonly |= BIT(i);
    DEBUG(D_expand) if (i == 1 && !sub2_honour_dollar && Ustrchr(sub[1], '$'))
      debug_printf_indent("WARNING: the second arg is NOT expanded,"
			" for security reasons\n");
    if (*s++ != '}') goto COND_FAILED_CURLY_END;

    /* Convert to numerical if required; we know that the names of all the
    conditions that compare numbers do not start with a letter. This just saves
    checking for them individually. */

    if (!isalpha(opname[0]) && yield)
      if (sub[i][0] == 0)
        {
        num[i] = 0;
        DEBUG(D_expand)
          debug_printf_indent("empty string cast to zero for numerical comparison\n");
        }
      else
        {
        num[i] = expanded_string_integer(sub[i], FALSE);
        if (expand_string_message) goto failout;
        }
    }

  /* Result not required */

  if (!yield) { next = s; goto out; }

  /* Do an appropriate comparison */

  switch(cond_type)
    {
    case ECOND_NUM_E:
    case ECOND_NUM_EE:
      tempcond = (num[0] == num[1]); break;

    case ECOND_NUM_G:
      tempcond = (num[0] > num[1]); break;

    case ECOND_NUM_GE:
      tempcond = (num[0] >= num[1]); break;

    case ECOND_NUM_L:
      tempcond = (num[0] < num[1]); break;

    case ECOND_NUM_LE:
      tempcond = (num[0] <= num[1]); break;

    case ECOND_STR_LT:
      tempcond = (Ustrcmp(sub[0], sub[1]) < 0); break;

    case ECOND_STR_LTI:
      tempcond = (strcmpic(sub[0], sub[1]) < 0); break;

    case ECOND_STR_LE:
      tempcond = (Ustrcmp(sub[0], sub[1]) <= 0); break;

    case ECOND_STR_LEI:
      tempcond = (strcmpic(sub[0], sub[1]) <= 0); break;

    case ECOND_STR_EQ:
      tempcond = (Ustrcmp(sub[0], sub[1]) == 0); break;

    case ECOND_STR_EQI:
      tempcond = (strcmpic(sub[0], sub[1]) == 0); break;

    case ECOND_STR_GT:
      tempcond = (Ustrcmp(sub[0], sub[1]) > 0); break;

    case ECOND_STR_GTI:
      tempcond = (strcmpic(sub[0], sub[1]) > 0); break;

    case ECOND_STR_GE:
      tempcond = (Ustrcmp(sub[0], sub[1]) >= 0); break;

    case ECOND_STR_GEI:
      tempcond = (strcmpic(sub[0], sub[1]) >= 0); break;

    case ECOND_MATCH:   /* Regular expression match */
      {
      const pcre2_code * re = regex_compile(sub[1],
		  sub_textonly & BIT(1) ? MCS_CACHEABLE : MCS_NOFLAGS,
		  &expand_string_message, pcre_gen_cmp_ctx);
      if (!re)
	goto failout;

      tempcond = regex_match_and_setup(re, sub[0], 0, -1);
      break;
      }

    case ECOND_MATCH_ADDRESS:  /* Match in an address list */
      rc = match_address_list(sub[0], TRUE,
#ifdef EXPAND_LISTMATCH_RHS
			      TRUE,
#else
			      FALSE,
#endif
			      &(sub[1]), NULL, -1, 0,
			      CUSS &lookup_value);
      goto MATCHED_SOMETHING;

    case ECOND_MATCH_DOMAIN:   /* Match in a domain list */
      rc = match_isinlist(sub[0], &(sub[1]), 0, &domainlist_anchor, NULL,
#ifdef EXPAND_LISTMATCH_RHS
			  MCL_DOMAIN,
#else
			  MCL_DOMAIN + MCL_NOEXPAND,
#endif
			  TRUE, CUSS &lookup_value);
      goto MATCHED_SOMETHING;

    case ECOND_MATCH_IP:       /* Match IP address in a host list */
      if (sub[0][0] != 0 && string_is_ip_address(sub[0], NULL) == 0)
	{
	expand_string_message = string_sprintf("%q is not an IP address",
	  sub[0]);
	goto failout;
	}
      else
	{
	unsigned int *nullcache = NULL;
	check_host_block cb;

	cb.host_name = US"";
	cb.host_address = sub[0];

	/* If the host address starts off ::ffff: it is an IPv6 address in
	IPv4-compatible mode. Find the IPv4 part for checking against IPv4
	addresses. */

	cb.host_ipv4 = (Ustrncmp(cb.host_address, "::ffff:", 7) == 0)?
	  cb.host_address + 7 : cb.host_address;

	rc = match_check_list(
		&sub[1],		/* the list */
		0,			/* separator character */
		&hostlist_anchor,	/* anchor pointer */
		&nullcache,		/* cache pointer */
		check_host,		/* function for testing */
		&cb,			/* argument for function */
#ifdef EXPAND_LISTMATCH_RHS
		MCL_HOST,
#else
		MCL_HOST + MCL_NOEXPAND,/* type of check */
#endif
		sub[0],			/* text for debugging */
		CUSS &lookup_value);	/* where to pass back data */
	}
      goto MATCHED_SOMETHING;

    case ECOND_MATCH_LOCAL_PART:
      rc = match_isinlist(sub[0], &(sub[1]), 0, &localpartlist_anchor, NULL,
#ifdef EXPAND_LISTMATCH_RHS
			  MCL_LOCALPART,
#else
			  MCL_LOCALPART+ MCL_NOEXPAND,
#endif
			  TRUE, CUSS &lookup_value);
      /* Fall through */
      /* VVVVVVVVVVVV */
      MATCHED_SOMETHING:
      switch(rc)
	{
	case OK:   tempcond = TRUE;  break;
	case FAIL: tempcond = FALSE; break;

	case DEFER:
	  expand_string_message = string_sprintf("unable to complete match "
	    "against %q: %s", sub[1], search_error_message);
	  goto failout;
	}

      break;

    /* Various "encrypted" comparisons. If the second string starts with
    "{" then an encryption type is given. Default to crypt() or crypt16()
    (build-time choice). */
    /* }-for-text-editors */

    case ECOND_CRYPTEQ:
    #ifndef SUPPORT_CRYPTEQ
      goto COND_FAILED_NOT_COMPILED;
    #else
      if (strncmpic(sub[1], US"{md5}", 5) == 0)
	{
	int sublen = Ustrlen(sub[1]+5);
	md5 base;
	uschar digest[16];

	md5_start(&base);
	md5_end(&base, sub[0], Ustrlen(sub[0]), digest);

	/* If the length that we are comparing against is 24, the MD5 digest
	is expressed as a base64 string. This is the way LDAP does it. However,
	some other software uses a straightforward hex representation. We assume
	this if the length is 32. Other lengths fail. */

	if (sublen == 24)
	  {
	  uschar *coded = b64encode(CUS digest, 16);
	  DEBUG(D_auth) debug_printf("crypteq: using MD5+B64 hashing\n"
	    "  subject=%s\n  crypted=%s\n", coded, sub[1]+5);
	  tempcond = (Ustrcmp(coded, sub[1]+5) == 0);
	  }
	else if (sublen == 32)
	  {
	  uschar coded[36];
	  for (int i = 0; i < 16; i++) sprintf(CS (coded+2*i), "%02X", digest[i]);
	  coded[32] = 0;
	  DEBUG(D_auth) debug_printf("crypteq: using MD5+hex hashing\n"
	    "  subject=%s\n  crypted=%s\n", coded, sub[1]+5);
	  tempcond = (strcmpic(coded, sub[1]+5) == 0);
	  }
	else
	  {
	  DEBUG(D_auth) debug_printf("crypteq: length for MD5 not 24 or 32: "
	    "fail\n  crypted=%s\n", sub[1]+5);
	  tempcond = FALSE;
	  }
	}

      else if (strncmpic(sub[1], US"{sha1}", 6) == 0)
	{
	int sublen = Ustrlen(sub[1]+6);
	hctx h;
	uschar digest[20];

	sha1_start(&h);
	sha1_end(&h, sub[0], Ustrlen(sub[0]), digest);

	/* If the length that we are comparing against is 28, assume the SHA1
	digest is expressed as a base64 string. If the length is 40, assume a
	straightforward hex representation. Other lengths fail. */

	if (sublen == 28)
	  {
	  uschar *coded = b64encode(CUS digest, 20);
	  DEBUG(D_auth) debug_printf("crypteq: using SHA1+B64 hashing\n"
	    "  subject=%s\n  crypted=%s\n", coded, sub[1]+6);
	  tempcond = (Ustrcmp(coded, sub[1]+6) == 0);
	  }
	else if (sublen == 40)
	  {
	  uschar coded[44];
	  for (int i = 0; i < 20; i++) sprintf(CS (coded+2*i), "%02X", digest[i]);
	  coded[40] = 0;
	  DEBUG(D_auth) debug_printf("crypteq: using SHA1+hex hashing\n"
	    "  subject=%s\n  crypted=%s\n", coded, sub[1]+6);
	  tempcond = (strcmpic(coded, sub[1]+6) == 0);
	  }
	else
	  {
	  DEBUG(D_auth) debug_printf("crypteq: length for SHA-1 not 28 or 40: "
	    "fail\n  crypted=%s\n", sub[1]+6);
	  tempcond = FALSE;
	  }
	}

      else   /* {crypt} or {crypt16} and non-{ at start */
	     /* }-for-text-editors */
	{
	int which = 0;
	uschar *coded;

	if (strncmpic(sub[1], US"{crypt}", 7) == 0)
	  {
	  sub[1] += 7;
	  which = 1;
	  }
	else if (strncmpic(sub[1], US"{crypt16}", 9) == 0)
	  {
	  sub[1] += 9;
	  which = 2;
	  }
	else if (sub[1][0] == '{')		/* }-for-text-editors */
	  {
	  expand_string_message = string_sprintf("unknown encryption mechanism "
	    "in %q", sub[1]);
	  goto failout;
	  }

	switch(which)
	  {
	  case 0:  coded = US DEFAULT_CRYPT(CS sub[0], CS sub[1]); break;
	  case 1:  coded = US crypt(CS sub[0], CS sub[1]); break;
	  default: coded = US crypt16(CS sub[0], CS sub[1]); break;
	  }

	#define STR(s) # s
	#define XSTR(s) STR(s)
	DEBUG(D_auth) debug_printf("crypteq: using %s()\n"
	  "  subject=%s\n  crypted=%s\n",
	  which == 0 ? XSTR(DEFAULT_CRYPT) : which == 1 ? "crypt" : "crypt16",
	  coded, sub[1]);
	#undef STR
	#undef XSTR

	/* If the encrypted string contains fewer than two characters (for the
	salt), force failure. Otherwise we get false positives: with an empty
	string the yield of crypt() is an empty string! */

	if (coded)
	  tempcond = Ustrlen(sub[1]) < 2 ? FALSE : Ustrcmp(coded, sub[1]) == 0;
	else if (errno == EINVAL)
	  tempcond = FALSE;
	else
	  {
	  expand_string_message = string_sprintf("crypt error: %s\n",
	    US strerror(errno));
	  goto failout;
	  }
	}
      break;
    #endif  /* SUPPORT_CRYPTEQ */

    case ECOND_INLIST:
    case ECOND_INLISTI:
      {
      const uschar * list = sub[1];
      int sep;
      uschar *save_iterate_item = iterate_item;
      int (*compare)(const uschar *, const uschar *);

      DEBUG(D_expand) debug_printf_indent("condition: %s  item: %s\n", opname, sub[0]);

      /* grab any listsep spec, then expand the list */

      sep = matchlist_parse_sep(&list);
      if (!(list = expand_string(list)))
	goto failout;

      tempcond = FALSE;
      compare = cond_type == ECOND_INLISTI
        ? strcmpic : (int (*)(const uschar *, const uschar *)) strcmp;

      while ((iterate_item = string_nextinlist(&list, &sep, NULL, 0)))
	{
	DEBUG(D_expand) debug_printf_indent(" compare %s\n", iterate_item);
        if (compare(sub[0], iterate_item) == 0)
          {
          tempcond = TRUE;
	  lookup_value = iterate_item;
          break;
          }
	}
      iterate_item = save_iterate_item;
      }

    }   /* Switch for comparison conditions */

  *yield = tempcond == testfor;
  next = s; goto out;    /* End of comparison conditions */


  /* and/or: computes logical and/or of several conditions */

  case ECOND_AND:
  case ECOND_OR:
  subcondptr = (yield == NULL) ? NULL : &tempcond;
  combined_cond = (cond_type == ECOND_AND);

  Uskip_whitespace(&s);
  if (*s++ != '{') goto COND_FAILED_CURLY_START;	/* }-for-text-editors */

  for (;;)
    {
    /* {-for-text-editors */
    if (Uskip_whitespace(&s) == '}') break;
    if (*s != '{')					/* }-for-text-editors */
      {
      expand_string_message = string_sprintf("each subcondition "
        "inside an \"%s{...}\" condition must be in its own {}", opname);
      goto failout;
      }

    if (!(s = eval_condition(s+1, resetok, subcondptr)))
      {
      expand_string_message = string_sprintf("%s inside \"%s{...}\" condition",
        expand_string_message, opname);
      goto failout;
      }
    Uskip_whitespace(&s);

    /* {-for-text-editors */
    if (*s++ != '}')
      {
      /* {-for-text-editors */
      expand_string_message = string_sprintf("missing } at end of condition "
        "inside %q group", opname);
      goto failout;
      }

    if (yield)
      if (cond_type == ECOND_AND)
        {
        combined_cond &= tempcond;
        if (!combined_cond) subcondptr = NULL;  /* once false, don't */
        }                                       /* evaluate any more */
      else
        {
        combined_cond |= tempcond;
        if (combined_cond) subcondptr = NULL;   /* once true, don't */
        }                                       /* evaluate any more */
    }

  if (yield) *yield = (combined_cond == testfor);
  next = ++s; goto out;


  /* forall/forany: iterates a condition with different values */

  case ECOND_FORALL:	  is_forany = FALSE;  is_json = FALSE; is_jsons = FALSE; goto FORMANY;
  case ECOND_FORANY:	  is_forany = TRUE;   is_json = FALSE; is_jsons = FALSE; goto FORMANY;
  case ECOND_FORALL_JSON: is_forany = FALSE;  is_json = TRUE;  is_jsons = FALSE; goto FORMANY;
  case ECOND_FORANY_JSON: is_forany = TRUE;   is_json = TRUE;  is_jsons = FALSE; goto FORMANY;
  case ECOND_FORALL_JSONS: is_forany = FALSE; is_json = TRUE;  is_jsons = TRUE;  goto FORMANY;
  case ECOND_FORANY_JSONS: is_forany = TRUE;  is_json = TRUE;  is_jsons = TRUE;  goto FORMANY;

  FORMANY:
    {
    const uschar * list;
    int sep;
    uschar *save_iterate_item = iterate_item;

    DEBUG(D_expand) debug_printf_indent("condition: %s\n", opname);

    /* First expand the list, apart from a leading change-of-separator
    on non-json lists */

    Uskip_whitespace(&s);
    if (*s++ != '{') goto COND_FAILED_CURLY_START;	/* }-for-text-editors */

    sep = is_json ? 0 : matchlist_parse_sep(&s);

    if (!(sub[0] = expand_string_internal(s,
      ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | (yield ? ESI_NOFLAGS : ESI_SKIPPING),
      &s, resetok, NULL)))
      goto failout;
    /* {-for-text-editors */
    if (*s++ != '}') goto COND_FAILED_CURLY_END;

    Uskip_whitespace(&s);
    if (*s++ != '{') goto COND_FAILED_CURLY_START;	/* }-for-text-editors */

    sub[1] = s;

    /* Call eval_condition once, with result discarded (as if scanning a
    "false" part). This allows us to find the end of the condition, because if
    the list is empty, we won't actually evaluate the condition for real. */

    if (!(s = eval_condition(sub[1], resetok, NULL)))
      {
      expand_string_message = string_sprintf("%s inside %q condition",
        expand_string_message, opname);
      goto failout;
      }
    Uskip_whitespace(&s);

    /* {-for-text-editors */
    if (*s++ != '}')
      {
      /* {-for-text-editors */
      expand_string_message = string_sprintf("missing } at end of condition "
        "inside %q", opname);
      goto failout;
      }

    /* Now scan the list, checking the condition for each item */

    if (yield) *yield = !testfor;
    list = sub[0];
    if (is_json) list = dewrap(string_copy(list), US"[]");
    while ((iterate_item = is_json
      ? json_nextinlist(&list) : string_nextinlist(&list, &sep, NULL, 0)))
      {
      if (is_jsons)
	if (!(iterate_item = dewrap(iterate_item, US"\"\"")))
	  {
	  expand_string_message =
	    string_sprintf("%s wrapping string result for extract jsons",
	      expand_string_message);
	  iterate_item = save_iterate_item;
	  goto failout;
	  }

      DEBUG(D_expand) debug_printf_indent("%s: $item = %q\n", opname, iterate_item);
      if (!eval_condition(sub[1], resetok, &tempcond))
        {
        expand_string_message = string_sprintf("%s inside %q condition",
          expand_string_message, opname);
        iterate_item = save_iterate_item;
        goto failout;
        }
      DEBUG(D_expand) debug_printf_indent("%s: condition evaluated to %s\n", opname,
        tempcond? "true":"false");

      if (yield) *yield = (tempcond == testfor);
      if (tempcond == is_forany) break;
      }

    iterate_item = save_iterate_item;
    next = s; goto out;
    }


  /* The bool{} expansion condition maps a string to boolean.
  The values supported should match those supported by the ACL condition
  (acl.c, ACLC_CONDITION) so that we keep to a minimum the different ideas
  of true/false.  Note that Router "condition" rules have a different
  interpretation, where general data can be used and only a few values
  map to FALSE.
  Note that readconf.c boolean matching, for boolean configuration options,
  only matches true/yes/false/no.
  The bool_lax{} condition matches the Router logic, which is much more
  liberal. */
  case ECOND_BOOL:
  case ECOND_BOOL_LAX:
    {
    uschar *sub_arg[1];
    uschar *t, *t2;
    uschar *ourname;
    size_t len;
    BOOL boolvalue = FALSE;

    if (Uskip_whitespace(&s) != '{') goto COND_FAILED_CURLY_START;	/* }-for-text-editors */
    ourname = cond_type == ECOND_BOOL_LAX ? US"bool_lax" : US"bool";
    switch(read_subs(sub_arg, 1, 1, &s,
	    yield ? ESI_NOFLAGS : ESI_SKIPPING, FALSE, ourname, resetok, NULL))
      {
      case 1: expand_string_message = string_sprintf(
                  "too few arguments or bracketing error for %s",
                  ourname);
      /*FALLTHROUGH*/
      case 2:
      case 3: goto failout;
      }
    t = sub_arg[0];
    Uskip_whitespace(&t);
    if ((len = Ustrlen(t)))
      {
      /* trailing whitespace: seems like a good idea to ignore it too */
      t2 = t + len - 1;
      while (isspace(*t2)) t2--;
      if (t2 != (t + len))
        {
        *++t2 = '\0';
        len = t2 - t;
        }
      }
    DEBUG(D_expand)
      debug_printf_indent("considering %s: %s\n", ourname, len ? t : US"<empty>");
    /* logic for the lax case from expand_check_condition(), which also does
    expands, and the logic is both short and stable enough that there should
    be no maintenance burden from replicating it. */
    if (len == 0)
      boolvalue = FALSE;
    else if (*t == '-'
	     ? Ustrspn(t+1, "0123456789") == len-1
	     : Ustrspn(t,   "0123456789") == len)
      {
      boolvalue = (Uatoi(t) == 0) ? FALSE : TRUE;
      /* expand_check_condition only does a literal string "0" check */
      if ((cond_type == ECOND_BOOL_LAX) && (len > 1))
        boolvalue = TRUE;
      }
    else if (strcmpic(t, US"true") == 0 || strcmpic(t, US"yes") == 0)
      boolvalue = TRUE;
    else if (strcmpic(t, US"false") == 0 || strcmpic(t, US"no") == 0)
      boolvalue = FALSE;
    else if (cond_type == ECOND_BOOL_LAX)
      boolvalue = TRUE;
    else
      {
      expand_string_message = string_sprintf("unrecognised boolean "
       "value %q", t);
      goto failout;
      }
    DEBUG(D_expand) debug_printf_indent("%s: condition evaluated to %s\n", ourname,
        boolvalue? "true":"false");
    if (yield) *yield = (boolvalue == testfor);
    next = s; goto out;
    }

#ifdef SUPPORT_SRS
  case ECOND_INBOUND_SRS:
    /* ${if inbound_srs {local_part}{secret}  {yes}{no}} */
    {
    uschar * sub[2];
    const pcre2_code * re;
    pcre2_match_data * md;
    PCRE2_SIZE * ovec;
    int quoting = 0;
    uschar cksum[4];
    BOOL boolvalue = FALSE;

    switch(read_subs(sub, 2, 2, CUSS &s,
	    yield ? ESI_NOFLAGS : ESI_SKIPPING, FALSE, name, resetok, NULL))
      {
      case 1: expand_string_message = US"too few arguments or bracketing "
	"error for inbound_srs";
      case 2:
      case 3: goto failout;
      }

    /* Match the given local_part against the SRS-encoded pattern */

    re = regex_must_compile(US"^(?i)SRS0=([^=]+)=([A-Z2-7]{2})=([^=]*)=(.*)$",
			    MCS_CASELESS | MCS_CACHEABLE, FALSE);
    md = pcre2_match_data_create(4+1, pcre_gen_ctx);
    if (pcre2_match(re, sub[0], PCRE2_ZERO_TERMINATED, 0, PCRE_EOPT,
		    md, pcre_gen_mtc_ctx) < 0)
      {
      DEBUG(D_expand) debug_printf("no match for SRS'd local-part pattern\n");
      goto srs_result;
      }
    ovec = pcre2_get_ovector_pointer(md);

    if (sub[0][0] == '"')
      quoting = 1;
    else for (uschar * s = sub[0]; *s; s++)
      if (!isalnum(*s) && Ustrchr(".!#$%&'*+-/=?^_`{|}~", *s) == NULL)
	{ quoting = 1; break; }
    if (quoting)
      DEBUG(D_expand) debug_printf_indent("auto-quoting local part\n");

    /* Record the (quoted, if needed) decoded recipient as $srs_recipient */

    srs_recipient = string_sprintf("%.*s%.*S%.*s@%.*S",   	/* lowercased */
		      quoting, "\"",
		      (int) (ovec[9]-ovec[8]), sub[0] + ovec[8],  /* substr 4 */
		      quoting, "\"",
		      (int) (ovec[7]-ovec[6]), sub[0] + ovec[6]); /* substr 3 */

    /* If a zero-length secret was given, we're done.  Otherwise carry on
    and validate the given SRS local_part againt our secret. */

    if (*sub[1])
      {
      /* check the timestamp */
	{
	struct timeval now;
	const uschar * ss = sub[0] + ovec[4];	/* substring 2, the timestamp */
	long d;
	int n;

	gettimeofday(&now, NULL);
	now.tv_sec /= 86400;			/* days since epoch */

	/* Decode substring 2 from base32 to a number */

	for (d = 0, n = ovec[5]-ovec[4]; n; n--)
	  {
	  const uschar * t = Ustrchr(base32_chars, *ss++);
	  d = d * 32 + (t - base32_chars);
	  }

	if (((now.tv_sec - d) & 0x3ff) > 10)	/* days since SRS generated */
	  {
	  DEBUG(D_expand) debug_printf("SRS too old\n");
	  goto srs_result;
	  }
	}

      /* check length of substring 1, the offered checksum */

      if (ovec[3]-ovec[2] != 4)
	{
	DEBUG(D_expand) debug_printf("SRS checksum wrong size\n");
	goto srs_result;
	}

      /* Hash the address with our secret, and compare that computed checksum
      with the one extracted from the arg */

      hmac_md5(sub[1], srs_recipient, cksum, sizeof(cksum));
      if (Ustrncmp(cksum, sub[0] + ovec[2], 4) != 0)
	{
	DEBUG(D_expand) debug_printf("SRS checksum mismatch\n");
	goto srs_result;
	}
      }
    boolvalue = TRUE;

srs_result:
    /* pcre2_match_data_free(md);	gen ctx needs no free */
    if (yield) *yield = (boolvalue == testfor);
    next = s; goto out;
    }
#endif /*SUPPORT_SRS*/

  /* Unknown condition */

  default:
    if (!expand_string_message || !*expand_string_message)
      expand_string_message = string_sprintf("unknown condition %q", opname);
    goto failout;
  }   /* End switch on condition type */

/* Missing braces at start and end of data */

COND_FAILED_CURLY_START:
expand_string_message = string_sprintf("missing { after %q", opname);
goto failout;

COND_FAILED_CURLY_END:
expand_string_message = string_sprintf("missing } at end of %q condition",
  opname);
goto failout;

/* A condition requires code that is not compiled */

#if !defined(SUPPORT_PAM) || !defined(RADIUS_CONFIG_FILE) || \
    !defined(LOOKUP_LDAP) || !defined(CYRUS_PWCHECK_SOCKET) || \
    !defined(SUPPORT_CRYPTEQ) || !defined(CYRUS_SASLAUTHD_SOCKET)
COND_FAILED_NOT_COMPILED:
expand_string_message = string_sprintf("support for %q not compiled",
  opname);
goto failout;
#endif

failout:
  next = NULL;
out:
  expand_level--;
  return next;
}




/*************************************************
*          Save numerical variables              *
*************************************************/

/* This function is called from items such as "if" that want to preserve and
restore the numbered variables.

Arguments:
  save_expand_string    points to an array of pointers to set
  save_expand_nlength   points to an array of ints for the lengths

Returns:                the value of expand max to save
*/

static int
save_expand_strings(const uschar **save_expand_nstring, int *save_expand_nlength)
{
for (int i = 0; i <= expand_nmax; i++)
  {
  save_expand_nstring[i] = expand_nstring[i];
  save_expand_nlength[i] = expand_nlength[i];
  }
return expand_nmax;
}



/*************************************************
*           Restore numerical variables          *
*************************************************/

/* This function restored saved values of numerical strings.

Arguments:
  save_expand_nmax      the number of strings to restore
  save_expand_string    points to an array of pointers
  save_expand_nlength   points to an array of ints

Returns:                nothing
*/

static void
restore_expand_strings(int save_expand_nmax,
  const uschar ** save_expand_nstring, const int * save_expand_nlength)
{
expand_nmax = save_expand_nmax;
for (int i = 0; i <= expand_nmax; i++)
  {
  expand_nstring[i] = save_expand_nstring[i];
  expand_nlength[i] = save_expand_nlength[i];
  }
}





/*************************************************
*            Handle yes/no substrings            *
*************************************************/

/* This function is used by ${if}, ${lookup} and ${extract} to handle the
alternative substrings that depend on whether or not the condition was true,
or the lookup or extraction succeeded. The substrings always have to be
expanded, to check their syntax, but "skipping" is set when the result is not
needed - this avoids unnecessary nested lookups.

Arguments:
  flags
   skipping       TRUE if we were skipping when this item was reached
  yes            TRUE if the first string is to be used, else use the second
  save_lookup    a value to put back into lookup_value before the 2nd expansion
  sptr           points to the input string pointer
  yieldptr       points to the output growable-string pointer
  type           "lookup", "if", "extract", "run", "env", "listextract" or
                 "certextract" for error message
  resetok	 if not NULL, pointer to flag - write FALSE if unsafe to reset
		the store.

Returns:         0 OK; lookup_value has been reset to save_lookup
                 1 expansion failed
                 2 expansion failed because of bracketing error
*/

static int
process_yesno(esi_flags flags, BOOL yes, uschar *save_lookup, const uschar **sptr,
  gstring ** yieldptr, uschar *type, BOOL *resetok)
{
int rc = 0;
const uschar * s = *sptr;    /* Local value */
const uschar * sub1, * sub2, * errwhere;

flags &= ESI_SKIPPING;		/* Ignore all buf the skipping flag */

/* If there are no following strings, we substitute the contents of $value for
lookups and for extractions in the success case. For the ${if item, the string
"true" is substituted. In the fail case, nothing is substituted for all three
items. */

if (skip_whitespace(&s) == '}')
  {
  if (type[0] == 'i')
    {
    if (yes && !(flags & ESI_SKIPPING))
      *yieldptr = string_catn(*yieldptr, US"true", 4);
    }
  else
    {
    if (yes && lookup_value && !(flags & ESI_SKIPPING))
      *yieldptr = string_cat(*yieldptr, lookup_value);
    lookup_value = save_lookup;
    }
  s++;
  goto RETURN;
  }

/* The first following string must be braced. */

if (*s++ != '{')
  {
  errwhere = US"'yes' part did not start with '{'";		/*}}*/
  goto FAILED_CURLY;
  }

/* Expand the first substring. Forced failures are noticed only if we actually
want this string. Set skipping in the call in the fail case (this will always
be the case if we were already skipping). */

sub1 = expand_string_internal(s,
  ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | (yes ? ESI_NOFLAGS : ESI_SKIPPING),
  &s, resetok, NULL);
if (sub1 == NULL && (yes || !f.expand_string_forcedfail)) goto FAILED;
f.expand_string_forcedfail = FALSE;
								/*{{*/
if (*s++ != '}')
  {
  errwhere = US"'yes' part did not end with '}'";
  goto FAILED_CURLY;
  }

/* If we want the first string, add it to the output */

if (yes)
  *yieldptr = string_cat(*yieldptr, sub1);

/* If this is called from a lookup/env or a (cert)extract, we want to restore
$value to what it was at the start of the item, so that it has this value
during the second string expansion. For the call from "if" or "run" to this
function, save_lookup is set to lookup_value, so that this statement does
nothing. */

lookup_value = save_lookup;

/* There now follows either another substring, or "fail", or nothing. This
time, forced failures are noticed only if we want the second string. We must
set skipping in the nested call if we don't want this string, or if we were
already skipping. */

if (skip_whitespace(&s) == '{')					/*}*/
  {
  esi_flags s_flags = ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags;
  if (yes) s_flags |= ESI_SKIPPING;
  sub2 = expand_string_internal(s+1, s_flags, &s, resetok, NULL);
  if (!sub2 && (!yes || !f.expand_string_forcedfail)) goto FAILED;
  f.expand_string_forcedfail = FALSE;				/*{*/
  if (*s++ != '}')
    {
    errwhere = US"'no' part did not start with '{'";		/*}*/
    goto FAILED_CURLY;
    }

  /* If we want the second string, add it to the output */

  if (!yes)
    *yieldptr = string_cat(*yieldptr, sub2);
  }
								/*{{*/
/* If there is no second string, but the word "fail" is present when the use of
the second string is wanted, set a flag indicating it was a forced failure
rather than a syntactic error. Swallow the terminating } in case this is nested
inside another lookup or if or extract. */

else if (*s != '}')
  {
  uschar name[256];
  /* deconst cast ok here as source is s anyway */
  s = US read_name(name, sizeof(name), s, US"_");
  if (Ustrcmp(name, "fail") == 0)
    {
    if (!yes && !(flags & ESI_SKIPPING))
      {
      Uskip_whitespace(&s);					/*{{*/
      if (*s++ != '}')
        {
	errwhere = US"did not close with '}' after forcedfail";
	goto FAILED_CURLY;
	}
      expand_string_message =
        string_sprintf("%q failed and \"fail\" requested", type);
      f.expand_string_forcedfail = TRUE;
      goto FAILED;
      }
    }
  else
    {
    expand_string_message =
      string_sprintf("syntax error in %q item - \"fail\" expected", type);
    goto FAILED;
    }
  }

/* All we have to do now is to check on the final closing brace. */

skip_whitespace(&s);						/*{{*/
if (*s++ != '}')
  {
  errwhere = US"did not close with '}'";
  goto FAILED_CURLY;
  }


RETURN:
/* Update the input pointer value before returning */
*sptr = s;
return rc;

FAILED_CURLY:
  /* Get here if there is a bracketing failure */
  expand_string_message = string_sprintf(
    "curly-bracket problem in conditional yes/no parsing: %s\n"
    " remaining string is '%s'", errwhere, --s);
  rc = 2;
  goto RETURN;

FAILED:
  /* Get here for other failures */
  rc = 1;
  goto RETURN;
}




/********************************************************
* prvs: Get last three digits of days since Jan 1, 1970 *
********************************************************/

/* This is needed to implement the "prvs" BATV reverse
   path signing scheme

Argument: integer "days" offset to add or substract to
          or from the current number of days.

Returns:  pointer to string containing the last three
          digits of the number of days since Jan 1, 1970,
          modified by the offset argument, NULL if there
          was an error in the conversion.

*/

static uschar *
prvs_daystamp(int day_offset)
{
uschar * days = store_get(32, GET_UNTAINTED);      /* Need at least 24 for cases */
(void)string_format(days, 32, TIME_T_FMT, 	   /* where TIME_T_FMT is %lld */
  (time(NULL) + day_offset*86400)/86400);
return (Ustrlen(days) >= 3) ? &days[Ustrlen(days)-3] : US"100";
}



/********************************************************
*   prvs: perform HMAC-SHA1 computation of prvs bits    *
********************************************************/

/* This is needed to implement the "prvs" BATV reverse
   path signing scheme

Arguments:
  address RFC2821 Address to use
      key The key to use (must be less than 64 characters
          in size)
  key_num Single-digit key number to use. Defaults to
          '0' when NULL.

Returns:  pointer to string containing the first three
          bytes of the final hash in hex format, NULL if
          there was an error in the process.
*/

static uschar *
prvs_hmac_sha1(const uschar * address, const uschar * key,
  const uschar * key_num, const uschar * daystamp)
{
gstring * hash_source;
uschar * p;
hctx h;
uschar innerhash[20], finalhash[20];
uschar innerkey[64],  outerkey[64];
uschar * finalhash_hex;

if (!key_num)
  key_num = US"0";

if (Ustrlen(key) > 64)
  return NULL;

hash_source = string_catn(NULL, key_num, 1);
hash_source = string_catn(hash_source, daystamp, 3);
hash_source = string_cat(hash_source, address);

DEBUG(D_expand)
  debug_printf_indent("prvs: hash source is '%Y'\n", hash_source);

memset(innerkey, 0x36, 64);
memset(outerkey, 0x5c, 64);

for (int i = 0; i < Ustrlen(key); i++)
  {
  innerkey[i] ^= key[i];
  outerkey[i] ^= key[i];
  }

chash_start(HMAC_SHA1, &h);
chash_mid(HMAC_SHA1, &h, innerkey);
chash_end(HMAC_SHA1, &h, hash_source->s, hash_source->ptr, innerhash);

chash_start(HMAC_SHA1, &h);
chash_mid(HMAC_SHA1, &h, outerkey);
chash_end(HMAC_SHA1, &h, innerhash, 20, finalhash);

/* Hashing is deemed sufficient to de-taint any input data */

p = finalhash_hex = store_get(40, GET_UNTAINTED);
for (int i = 0; i < 3; i++)
  {
  *p++ = hex_digits[(finalhash[i] & 0xf0) >> 4];
  *p++ = hex_digits[finalhash[i] & 0x0f];
  }
*p = '\0';

return finalhash_hex;
}




/*************************************************
*        Join a file onto the output string      *
*************************************************/

/* This is used for readfile/readsock and after a run expansion.
It joins the contents of a file onto the output string, globally replacing
newlines with a given string (optionally).

Arguments:
  f            the FILE
  yield        pointer to the expandable string struct
  eol          newline replacement string, or NULL

Returns:       new pointer for expandable string, terminated if non-null
*/

gstring *
cat_file(FILE * f, gstring * yield, const uschar * eol)
{
uschar buffer[1024];

while (Ufgets(buffer, sizeof(buffer), f))
  {
  int len = Ustrlen(buffer);
  if (eol && buffer[len-1] == '\n') len--;
  yield = string_catn(yield, buffer, len);
  if (eol && buffer[len])
    yield = string_cat(yield, eol);
  }
return yield;
}


#ifndef DISABLE_TLS
gstring *
cat_file_tls(void * tls_ctx, gstring * yield, const uschar * eol)
{
int rc;
uschar buffer[1024];

/*XXX could we read direct into a pre-grown string? */

while ((rc = tls_read(tls_ctx, buffer, sizeof(buffer))) > 0)
  for (uschar * s = buffer; rc--; s++)
    yield = eol && *s == '\n'
      ? string_cat(yield, eol) : string_catn(yield, s, 1);

/* We assume that all errors, and any returns of zero bytes,
are actually EOF. */

return yield;
}
#endif


/*************************************************
*          Evaluate numeric expression           *
*************************************************/

static inline void
eval_dbg_op_2(const uschar * op, int_eximarith_t a, int_eximarith_t b)
{
DEBUG(D_expand)
  debug_printf_indent("eval " PR_EXIM_ARITH " %s " PR_EXIM_ARITH, a, op, b);
}
static inline void
eval_dbg_res(int_eximarith_t res)
{
DEBUG(D_expand) debug_printf(" => " PR_EXIM_ARITH "\n", res);
}
static inline void
eval_dbg(const uschar * op, int_eximarith_t res)
{
DEBUG(D_expand)
  debug_printf_indent("eval '%s' res: " PR_EXIM_ARITH "\n", op, res);
}



/* This is a set of mutually recursive functions that evaluate an arithmetic
expression involving + - * / % & | ^ ~ << >> and parentheses. The only one of
these functions that is called from elsewhere is eval_expr, whose interface is:

Arguments:
  sptr        pointer to the pointer to the string - gets updated
  decimal     TRUE if numbers are to be assumed decimal
  error       pointer to where to put an error message - must be NULL on input
  endket      TRUE if ')' must terminate - FALSE for external call

Returns:      on success: the value of the expression, with *error still NULL
              on failure: an undefined value, with *error = a message
*/

static int_eximarith_t eval_op_or(uschar **, BOOL, uschar **);


static int_eximarith_t
eval_expr(uschar **sptr, BOOL decimal, uschar **error, BOOL endket)
{
uschar * s = *sptr;
int_eximarith_t x = eval_op_or(&s, decimal, error);

if (!*error)
  if (endket)
    if (*s != ')')
      *error = US"expecting closing parenthesis";
    else
      while (isspace(*++s)) ;
  else if (*s)
    *error = US"expecting operator";
*sptr = s;
return x;
}


static int_eximarith_t
eval_number(uschar **sptr, BOOL decimal, uschar **error)
{
int c;
int_eximarith_t n;
uschar * s = *sptr;

expand_level++;
  {
  if (isdigit((c = Uskip_whitespace(&s))))
    {
    int count;
    (void)sscanf(CS s, (decimal? SC_EXIM_DEC "%n" : SC_EXIM_ARITH "%n"), &n, &count);
    s += count;
    switch (tolower(*s))
      {
      default: break;
      case 'k': n *= 1024; s++; break;
      case 'm': n *= 1024*1024; s++; break;
      case 'g': n *= 1024*1024*1024; s++; break;
      }
    Uskip_whitespace(&s);
    eval_dbg(US"number", n);
    }
  else if (c == '(')
    {
    s++;
    n = eval_expr(&s, decimal, error, 1);
    eval_dbg(US"paren", n);
    }
  else
    {
    *error = US"expecting number or opening parenthesis";
    n = 0;
    }
  *sptr = s;
  }
expand_level--;
return n;
}


static int_eximarith_t
eval_op_unary(uschar **sptr, BOOL decimal, uschar **error)
{
uschar * s = *sptr;
int_eximarith_t x;

Uskip_whitespace(&s);
if (*s == '+' || *s == '-' || *s == '~')
  {
  int op = *s++;
  expand_level++;
    {
    x = eval_op_unary(&s, decimal, error);
    if (op == '-')
      {
      x = -x;
      eval_dbg(US"negate", x);
      }
    else if (op == '~')
      {
      x = ~x;
      eval_dbg(US"invert", x);
      }
    else
      eval_dbg(US"unary-plus", x);
    expand_level--;
    }
  }
else
  x = eval_number(&s, decimal, error);

*sptr = s;
return x;
}


static int_eximarith_t
eval_op_mult(uschar **sptr, BOOL decimal, uschar **error)
{
uschar * s = *sptr;
int_eximarith_t x = eval_op_unary(&s, decimal, error);
if (!*error)
  {
  while (*s == '*' || *s == '/' || *s == '%')
    {
    expand_level++;
      {
      int op = *s++;
      int_eximarith_t y = eval_op_unary(&s, decimal, error);
      if (*error) break;

      /* SIGFPE both on div/mod by zero and on INT_MIN / -1, which would give
      a value of INT_MAX+1. Note that INT_MIN * -1 gives INT_MIN for me, which
      is a bug somewhere in [gcc 4.2.1, FreeBSD, amd64].  In fact, -N*-M where
      -N*M is INT_MIN will yield INT_MIN.
      Since we don't support floating point, this is somewhat simpler.
      Ideally, we'd return an error, but since we overflow for all other
      arithmetic, consistency suggests otherwise, but what's the correct value
      to use?  There is none.
      The C standard guarantees overflow for unsigned arithmetic but signed
      overflow invokes undefined behaviour; in practice, this is overflow
      except for converting INT_MIN to INT_MAX+1.  We also can't guarantee
      that long/longlong larger than int are available, or we could just work
      with larger types.  We should consider whether to guarantee 32bit eval
      and 64-bit working variables, with errors returned.  For now ...
      So, the only SIGFPEs occur with a non-shrinking div/mod, thus -1; we
      can just let the other invalid results occur otherwise, as they have
      until now.  For this one case, we can coerce.  */

      if (y == -1 && x == EXIM_ARITH_MIN && op != '*')
	{
	DEBUG(D_expand)
	  debug_printf("Integer exception dodging: " PR_EXIM_ARITH "%c-1 coerced to " PR_EXIM_ARITH "\n",
	      EXIM_ARITH_MIN, op, EXIM_ARITH_MAX);
	x = EXIM_ARITH_MAX;
	continue;
	}
      if (op == '*')
	{
	eval_dbg_op_2(US"mul", x, y);
	x *= y;
	}
      else
	{
	if (y == 0)
	  {
	  *error = (op == '/') ? US"divide by zero" : US"modulo by zero";
	  x = 0;
	  break;
	  }
	if (op == '/')
	  {
	  eval_dbg_op_2(US"div", x, y);
	  x /= y;
	  }
	else
	  {
	  eval_dbg_op_2(US"mod", x, y);
	  x %= y;
	  }
	}
      eval_dbg_res(x);
      }
    expand_level--;
    }
  }
*sptr = s;
return x;
}


static int_eximarith_t
eval_op_sum(uschar **sptr, BOOL decimal, uschar **error)
{
uschar * s = *sptr;
int_eximarith_t x = eval_op_mult(&s, decimal, error);
if (!*error)
  {
  BOOL isplus;
  while ((isplus = (*s == '+')) || *s == '-')
    {
    s++;
    expand_level++;
      {
      int_eximarith_t y = eval_op_mult(&s, decimal, error);
      if (*error) break;
      if (  (x >=   EXIM_ARITH_MAX/2  && x >=   EXIM_ARITH_MAX/2)
	 || (x <= -(EXIM_ARITH_MAX/2) && y <= -(EXIM_ARITH_MAX/2)))
	{			/* over-conservative check */
	*error = isplus ? US"overflow in sum" : US"overflow in difference";
	break;
	}
      eval_dbg_op_2(isplus ? US"plus" : US"minus", x, y);
      if (isplus) x += y; else x -= y;
      eval_dbg_res(x);
      }
    expand_level--;
    }
  }
*sptr = s;
return x;
}


static int_eximarith_t
eval_op_shift(uschar **sptr, BOOL decimal, uschar **error)
{
uschar * s = *sptr;
int_eximarith_t x = eval_op_sum(&s, decimal, error);
if (!*error)
  {
  while ((*s == '<' || *s == '>') && s[1] == s[0])
    {
    int op = *s++;
    s++;
    expand_level++;
      {
      int_eximarith_t y = eval_op_sum(&s, decimal, error);
      if (*error) break;
      eval_dbg_op_2(US"shift", x, y);
      if (op == '<') x <<= y; else x >>= y;
      eval_dbg_res(x);
      }
    expand_level--;
    }
  }
*sptr = s;
return x;
}


static int_eximarith_t
eval_op_and(uschar **sptr, BOOL decimal, uschar **error)
{
uschar * s = *sptr;
int_eximarith_t x;

x = eval_op_shift(&s, decimal, error);
if (!*error)
  while (*s == '&')
    {
    s++;
    expand_level++;
      {
      int_eximarith_t y = eval_op_shift(&s, decimal, error);
      if (*error) break;
      eval_dbg_op_2(US"and", x, y);
      x &= y;
      eval_dbg_res(x);
      }
    expand_level--;
    }
*sptr = s;
return x;
}


static int_eximarith_t
eval_op_xor(uschar **sptr, BOOL decimal, uschar **error)
{
uschar * s = *sptr;
int_eximarith_t x = eval_op_and(&s, decimal, error);
if (!*error)
  while (*s == '^')
    {
    s++;
    expand_level++;
      {
      int_eximarith_t y = eval_op_and(&s, decimal, error);
      if (*error) break;
      eval_dbg_op_2(US"xor", x, y);
      x ^= y;
      eval_dbg_res(x);
      }
    expand_level--;
    }
*sptr = s;
return x;
}


static int_eximarith_t
eval_op_or(uschar **sptr, BOOL decimal, uschar **error)
{
uschar * s = *sptr;
int_eximarith_t x = eval_op_xor(&s, decimal, error);
if (!*error)
  while (*s == '|')
    {
    s++;
    expand_level++;
      {
      int_eximarith_t y = eval_op_xor(&s, decimal, error);
      if (*error) break;
      eval_dbg_op_2(US"or", x, y);
      x |= y;
      eval_dbg_res(x);
      }
    expand_level--;
    }
*sptr = s;
return x;
}



/************************************************/
/* Comparison operation for sort expansion.  We need to avoid
re-expanding the fields being compared, so need a custom routine.

Arguments:
 cond_type		Comparison operator code
 leftarg, rightarg	Arguments for comparison

Return true iff (leftarg compare rightarg)
*/

static BOOL
sortsbefore(int cond_type, BOOL alpha_cond,
  const uschar * leftarg, const uschar * rightarg)
{
int_eximarith_t l_num, r_num;

if (!alpha_cond)
  {
  l_num = expanded_string_integer(leftarg, FALSE);
  if (expand_string_message) return FALSE;
  r_num = expanded_string_integer(rightarg, FALSE);
  if (expand_string_message) return FALSE;

  switch (cond_type)
    {
    case ECOND_NUM_G:	return l_num >  r_num;
    case ECOND_NUM_GE:	return l_num >= r_num;
    case ECOND_NUM_L:	return l_num <  r_num;
    case ECOND_NUM_LE:	return l_num <= r_num;
    default: break;
    }
  }
else
  switch (cond_type)
    {
    case ECOND_STR_LT:	return Ustrcmp (leftarg, rightarg) <  0;
    case ECOND_STR_LTI:	return strcmpic(leftarg, rightarg) <  0;
    case ECOND_STR_LE:	return Ustrcmp (leftarg, rightarg) <= 0;
    case ECOND_STR_LEI:	return strcmpic(leftarg, rightarg) <= 0;
    case ECOND_STR_GT:	return Ustrcmp (leftarg, rightarg) >  0;
    case ECOND_STR_GTI:	return strcmpic(leftarg, rightarg) >  0;
    case ECOND_STR_GE:	return Ustrcmp (leftarg, rightarg) >= 0;
    case ECOND_STR_GEI:	return strcmpic(leftarg, rightarg) >= 0;
    default: break;
    }
return FALSE;	/* should not happen */
}


/* Expand a named list.  Return false on failure. */
static gstring *
expand_listnamed(gstring * yield, const uschar * name, const uschar * listtype)
{
tree_node *t = NULL;
const uschar * list;
int sep = 0;
uschar * item;
BOOL needsep = FALSE;
#define LISTNAMED_BUF_SIZE 256
uschar b[LISTNAMED_BUF_SIZE];
uschar * buffer = b;

if (*name == '+') name++;
if (!listtype)		/* no-argument version */
  {
  if (  !(t = tree_search(addresslist_anchor, name))
     && !(t = tree_search(domainlist_anchor,  name))
     && !(t = tree_search(hostlist_anchor,    name)))
    t = tree_search(localpartlist_anchor, name);
  }
else switch(*listtype)	/* specific list-type version */
  {
  case 'a': t = tree_search(addresslist_anchor,   name); break;
  case 'd': t = tree_search(domainlist_anchor,    name); break;
  case 'h': t = tree_search(hostlist_anchor,      name); break;
  case 'l': t = tree_search(localpartlist_anchor, name); break;
  default:
    expand_string_message = US"bad suffix on \"list\" operator";
    return yield;
  }

if(!t)
  {
  expand_string_message = string_sprintf("%q is not a %snamed list",
    name, !listtype?""
      : *listtype=='a'?"address "
      : *listtype=='d'?"domain "
      : *listtype=='h'?"host "
      : *listtype=='l'?"localpart "
      : 0);
  return yield;
  }

list = ((namedlist_block *)(t->data.ptr))->string;

/* The list could be quite long so we (re)use a buffer for each element
rather than getting each in new memory */

if (is_tainted(list)) buffer = store_get(LISTNAMED_BUF_SIZE, GET_TAINTED);
while ((item = string_nextinlist(&list, &sep, buffer, LISTNAMED_BUF_SIZE)))
  {
  const uschar * buf = US" : ";
  if (needsep)
    yield = string_catn(yield, buf, 3);
  else
    needsep = TRUE;

  if (*item == '+')	/* list item is itself a named list */
    {
    yield = expand_listnamed(yield, item, listtype);
    if (expand_string_message)
      return yield;
    }

  else if (sep != ':')	/* item from non-colon-sep list, re-quote for colon list-separator */
    {
    char tok[3];
    tok[0] = sep; tok[1] = ':'; tok[2] = 0;

    for(char * cp; cp = strpbrk(CCS item, tok); item = US cp)
      {
      yield = string_catn(yield, item, cp - CS item);
      if (*cp++ == ':')	/* colon in a non-colon-sep list item, needs doubling */
	yield = string_catn(yield, US"::", 2);
      else		/* sep in item; should already be doubled; emit once */
	{
	yield = string_catn(yield, US tok, 1);
	if (*cp == sep) cp++;
	}
      }
    yield = string_cat(yield, item);
    }
  else
    yield = string_cat(yield, item);
  }
return yield;
}



/************************************************/
static void
debug_expansion_interim(const uschar * what, const uschar * value, int nchar,
  esi_flags flags)
{
debug_printf_indent("%V", "K");

for (int fill = 11 - Ustrlen(what); fill > 0; fill--)
  debug_printf("%V", "-");

debug_printf("%s: %.*W\n", what, nchar, value);
if (nchar > 0 && is_tainted(value))
  debug_printf_indent("%V          %V(tainted)\n",
    flags & ESI_SKIPPING ? "|" : " ", "\\__");
}


/************************************************/
#ifdef EXIM_PERL

const misc_module_info *
perl_startup(const uschar * startup_pl)
{
const misc_module_info * mi = NULL;
uschar * errstr;

expand_level++;

if (!startup_pl)
  expand_string_message = US"A setting of perl_startup is needed when "
    "using the Perl interpreter";

else if (!(mi = misc_mod_find(US"perl", &errstr)))
  expand_string_message =
    string_sprintf("failed to locate perl module: %s", errstr);

else if (!opt_perl_started)
  {
  uschar * initerror;
  typedef uschar * (*fn_t)(const uschar *);

  DEBUG(D_any) debug_printf_indent("Starting Perl interpreter\n");
  if ((initerror = (((fn_t *) mi->functions)[PERL_STARTUP]) (startup_pl)))
    {
    expand_string_message =
      string_sprintf("error in perl_startup code: %s\n", initerror);
    mi = NULL;
    }
  opt_perl_started = TRUE;
  }

expand_level--;
return mi;
}

#endif	/*EXIM_PERL*/

/*************************************************
*                 Expand string                  *
*************************************************/

/* Returns either an unchanged string, or the expanded string in stacking pool
store. Interpreted sequences are:

   \...                    normal escaping rules
   $name                   substitutes the variable
   ${name}                 ditto
   ${op:string}            operates on the expanded string value
   ${item{arg1}{arg2}...}  expands the args and then does the business
                             some literal args are not enclosed in {}

There are now far too many operators and item types to make it worth listing
them here in detail any more.

We use an internal routine recursively to handle embedded substrings. The
external function follows. The yield is NULL if the expansion failed, and there
are two cases: if something collapsed syntactically, or if "fail" was given
as the action on a lookup failure. These can be distinguished by looking at the
variable expand_string_forcedfail, which is TRUE in the latter case.

The skipping flag is set true when expanding a substring that isn't actually
going to be used (after "if" or "lookup") and it prevents lookups from
happening lower down.

Store usage: At start, a store block of the length of the input plus 64
is obtained. This is expanded as necessary by string_cat(), which might have to
get a new block, or might be able to expand the original. At the end of the
function we can release any store above that portion of the yield block that
was actually used. In many cases this will be optimal.

However: if the first item in the expansion is a variable name or header name,
we reset the store before processing it; if the result is in fresh store, we
use that without copying. This is helpful for expanding strings like
$message_headers which can get very long.

There's a problem if a ${dlfunc item has side-effects that cause allocation,
since resetting the store at the end of the expansion will free store that was
allocated by the plugin code as well as the slop after the expanded string. So
we skip any resets if ${dlfunc } has been used. The same applies for ${acl }
and, given the acl condition, ${if }. This is an unfortunate consequence of
string expansion becoming too powerful.

Arguments:
  s		  the string to be expanded
  flags
   brace_ends     expansion is to stop at }
   honour_dollar  TRUE if $ is to be expanded,
                  FALSE if it's just another character
   skipping       TRUE for recursive calls when the value isn't actually going
                  to be used (to allow for optimisation)
   exists_only	  return as soon as we have a char, for optimisation
  left           if not NULL, a pointer to the first character after the
                 expansion is placed here (typically used with brace_ends)
  resetok_p	 if not NULL, pointer to flag - write FALSE if unsafe to reset
		 the store.
  textonly_p	 if not NULL, pointer to flag - write bool for only-met-text

Returns:         NULL if expansion fails:
                   expand_string_forcedfail is set TRUE if failure was forced
                   expand_string_message contains a textual error message
                 a pointer to the expanded string on success
*/

static uschar *
expand_string_internal(const uschar * s, esi_flags flags, const uschar ** left,
  BOOL *resetok_p, BOOL * textonly_p)
{
rmark reset_point = store_mark();
gstring * yield = NULL;
int item_type;
const uschar * orig_string = s;
const uschar * save_expand_nstring[EXPAND_MAXN+1];
int save_expand_nlength[EXPAND_MAXN+1];
BOOL resetok = TRUE, first = TRUE, textonly = TRUE;

expand_level++;
f.expand_string_forcedfail = FALSE;
expand_string_message = US"";

if (is_tainted(s))
  {
  expand_string_message =
    string_sprintf("attempt to expand tainted string '%s'", s);
  log_write(0, LOG_MAIN|LOG_PANIC, "%s", expand_string_message);
  goto EXPAND_FAILED;
  }

 {
  int len = Ustrlen(s);
  if (len) yield = string_get(len + 64);
 }

while (*s)	/* known to be untainted */
  {
  uschar name[256];

  if (flags & ESI_EXISTS_ONLY && gstring_length(yield) > 0) break;

  DEBUG(D_expand)
    {
    debug_printf_indent("%V%V%s: %W\n",
      first ? "/" : "K",
      flags & ESI_SKIPPING ? "---" : "",
      flags & ESI_SKIPPING ? "scanning" : "considering", s);
    first = FALSE;
    }

  /* \ escapes the next character, which must exist, or else
  the expansion fails. There's a special escape, \N, which causes
  copying of the subject verbatim up to the next \N. Otherwise,
  the escapes are the standard set. */

  if (*s == '\\')
    {
    if (s[1] == 0)
      {
      expand_string_message = US"\\ at end of string";
      goto EXPAND_FAILED;
      }

    if (s[1] == 'N')
      {
      const uschar * t = s + 2;
      for (s = t; *s ; s++) if (*s == '\\' && s[1] == 'N') break;

      DEBUG(D_expand)
	debug_expansion_interim(US"protected", t, (int)(s - t), flags);
      if (!(flags & ESI_SKIPPING))
	yield = string_catn(yield, t, s - t);
      if (*s) s += 2;
      }
    else
      {
      uschar ch[1];
      DEBUG(D_expand)
	debug_printf_indent("%Vbackslashed: '\\%c'\n", "K", s[1]);
      ch[0] = string_interpret_escape(&s);
      if (!(flags & ESI_SKIPPING))
	yield = string_catn(yield, ch, 1);
      s++;
      }
    continue;
    }

									/*{{*/
  /* Anything other than $ is just copied verbatim, unless we are
  looking for a terminating } character. */

  if (flags & ESI_BRACE_ENDS && *s == '}') break;

  if (*s != '$' || !(flags & ESI_HONOR_DOLLAR))
    {
    int i = 1;								/*{*/
    for (const uschar * t = s+1;
	*t && *t != '$' && *t != '}' && *t != '\\'; t++) i++;

    DEBUG(D_expand) debug_expansion_interim(US"text", s, i, flags);

    if (!(flags & ESI_SKIPPING))
      yield = string_catn(yield, s, i);
    s += i;
    continue;
    }
  textonly = FALSE;

  /* No { after the $ - must be a plain name or a number for string
  match variable. There has to be a fudge for variables that are the
  names of header fields preceded by "$header_" because header field
  names can contain any printing characters except space and colon.
  For those that don't like typing this much, "$h_" is a synonym for
  "$header_". A non-existent header yields a NULL value; nothing is
  inserted. */	/*}*/

  if (isalpha(*++s))
    {
    const uschar * value;
    int newsize = 0;
    gstring * g = NULL;
    uschar * t;

    s = read_name(name, sizeof(name), s, US"_");

    /* If this is the first thing to be expanded, release the pre-allocated
    buffer. */

    if (!(flags & ESI_SKIPPING))
      if (!yield)
	g = store_get(sizeof(gstring), GET_UNTAINTED);
      else if (yield->ptr == 0)
	{
	if (resetok) reset_point = store_reset(reset_point);
	yield = NULL;
	reset_point = store_mark();
	g = store_get(sizeof(gstring), GET_UNTAINTED);	/* alloc _before_ calling find_variable() */
	}

    /* Header */

    if (  ( *(t = name) == 'h'
          || (*t == 'r' || *t == 'l' || *t == 'b') && *++t == 'h'
	  )
       && (*++t == '_' || Ustrncmp(t, "eader_", 6) == 0)
       )
      {
      unsigned flags = *name == 'r' ? FH_WANT_RAW
		      : *name == 'l' ? FH_WANT_RAW|FH_WANT_LIST
		      : 0;
      const uschar * charset = *name == 'b' ? NULL : headers_charset;

      s = read_header_name(name, sizeof(name), s);
      value = find_header(name, &newsize, flags, charset);

      /* If we didn't find the header, and the header contains a closing brace
      character, this may be a user error where the terminating colon
      has been omitted. Set a flag to adjust the error message in this case.
      But there is no error here - nothing gets inserted. */

      if (!value)
        {								/*{*/
        if (Ustrchr(name, '}')) malformed_header = TRUE;
        continue;
        }
      }

    /* Variable */

    else if (!(value = find_variable(name, flags, &newsize)))
      {
      expand_string_message =
	string_sprintf("unknown variable name %q", name);
	check_variable_error_message(name);
      goto EXPAND_FAILED;
      }

    /* If the data is known to be in a new buffer, newsize will be set to the
    size of that buffer. If this is the first thing in an expansion string,
    yield will be NULL; just point it at the new store instead of copying. Many
    expansion strings contain just one reference, so this is a useful
    optimization, especially for humungous headers.  We need to use a gstring
    structure that is not allocated after that new-buffer, else a later store
    reset in the middle of the buffer will make it inaccessible. */

    if (flags & ESI_SKIPPING)
      {
      DEBUG(D_expand)
	debug_expansion_interim(US"var", name, Ustrlen(name), flags);
      }
    else
      {
      int len = Ustrlen(value);
      DEBUG(D_expand) debug_expansion_interim(US"value", value, len, flags);
      if (!yield && newsize != 0)
	{
	yield = g;
	yield->size = newsize;
	yield->ptr = len;
	yield->s = US value; /* known to be in new store i.e. a copy, so deconst safe */
	}
      else
	yield = string_catn(yield, value, len);
      }

    continue;
    }

  if (isdigit(*s))		/* A $<n> variable */
    {
    int n;
    s = read_cnumber(&n, s);
    if (n >= 0 && n <= expand_nmax)
      {
      DEBUG(D_expand) debug_expansion_interim(US"value", expand_nstring[n], expand_nlength[n], flags);
      if (!(flags & ESI_SKIPPING))
	yield = string_catn(yield, expand_nstring[n], expand_nlength[n]);
      }
    continue;
    }

  /* Otherwise, if there's no '{' after $ it's an error. */		/*}*/

  if (*s != '{')							/*}*/
    {
    expand_string_message = US"$ not followed by letter, digit, or {";	/*}*/
    goto EXPAND_FAILED;
    }

  /* After { there can be various things, but they all start with
  an initial word, except for a number for a string match variable. */	/*}*/

  if (isdigit(*++s))
    {
    int n;
    s = read_cnumber(&n, s);						/*{{*/
    if (*s++ != '}')
      {
      expand_string_message = US"} expected after number";
      goto EXPAND_FAILED;
      }
    if (n >= 0 && n <= expand_nmax)
      {
      DEBUG(D_expand) debug_expansion_interim(US"value", expand_nstring[n], expand_nlength[n], flags);
      if (!(flags & ESI_SKIPPING))
	yield = string_catn(yield, expand_nstring[n], expand_nlength[n]);
      }
    continue;
    }

  if (!isalpha(*s))
    {
    expand_string_message = US"letter or digit expected after ${";	/*}*/
    goto EXPAND_FAILED;
    }

  /* Allow "-" in names to cater for substrings with negative
  arguments. Since we are checking for known names after { this is
  OK. */								/*}*/

  s = read_name(name, sizeof(name), s, US"_-");
  item_type = chop_match(name, item_table, nelem(item_table));

  /* Switch on item type.  All nondefault choices should "continue* when
  skipping, but "break" otherwise so we get debug output for the item
  expansion. */
  {
  int expansion_start = gstring_length(yield);
  switch(item_type)
    {
    /* Call an ACL from an expansion.  We feed data in via $acl_arg1 - $acl_arg9.
    If the ACL returns accept or reject we return content set by "message ="
    There is currently no limit on recursion; this would have us call
    acl_check_internal() directly and get a current level from somewhere.
    See also the acl expansion condition ECOND_ACL and the traditional
    acl modifier ACLC_ACL.
    Assume that the function has side-effects on the store that must be preserved.
    */

    case EITEM_ACL:
      /* ${acl {name} {arg1}{arg2}...} */
      {
      uschar * sub[10];	/* name + arg1-arg9 (which must match number of acl_arg[]) */
      uschar * user_msg;
      int rc;

      switch(read_subs(sub, nelem(sub), 1, &s, flags, TRUE, name, &resetok, NULL))
        {
	case -1: continue;		/* skipping */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      resetok = FALSE;
      switch(rc = eval_acl(sub, nelem(sub), &user_msg))
	{
	case OK:
	case FAIL:
	  DEBUG(D_expand)
	    debug_printf_indent("acl expansion yield: %s\n", user_msg);
	  if (user_msg)
            yield = string_cat(yield, user_msg);
	  break;

	case DEFER:
          f.expand_string_forcedfail = TRUE;
	  /*FALLTHROUGH*/
	default:
          expand_string_message = string_sprintf("%s from acl %q",
	    rc_names[rc], sub[0]);
	  goto EXPAND_FAILED;
	}
      break;
      }

    case EITEM_AUTHRESULTS:
      /* ${authresults {mysystemname}} */
      {
      uschar * sub_arg[1];

      switch(read_subs(sub_arg, nelem(sub_arg), 1, &s, flags, TRUE, name, &resetok, NULL))
        {
	case -1: continue;	/* If skipping, we don't actually do anything */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      yield = string_append(yield, 3,
			US"Authentication-Results: ", sub_arg[0], US"; none");
      yield->ptr -= 6;			/* ignore tha ": none" for now */

      yield = authres_local(yield, sub_arg[0]);
      yield = authres_iprev(yield);
      yield = authres_smtpauth(yield);
      yield = misc_mod_authres(yield);
      break;
      }

    /* Handle conditionals - preserve the values of the numerical expansion
    variables in case they get changed by a regular expression match in the
    condition. If not, they retain their external settings. At the end
    of this "if" section, they get restored to their previous values. */

    case EITEM_IF:
      {
      BOOL cond = FALSE;
      const uschar *next_s;
      int save_expand_nmax =
        save_expand_strings(save_expand_nstring, save_expand_nlength);
      uschar * save_lookup_value = lookup_value;

      Uskip_whitespace(&s);
      if (!(next_s = eval_condition(s, &resetok, flags & ESI_SKIPPING ? NULL : &cond)))
	goto EXPAND_FAILED;  /* message already set */

      DEBUG(D_expand)
	{
	debug_expansion_interim(US"condition", s, (int)(next_s - s), flags);
	debug_expansion_interim(US"result",
	  cond ? US"true" : US"false", cond ? 4 : 5, flags);
	}

      s = next_s;

      /* The handling of "yes" and "no" result strings is now in a separate
      function that is also used by ${lookup} and ${extract} and ${run}. */

      switch(process_yesno(
               flags,			/* were previously skipping */
               cond,			/* success/failure indicator */
               lookup_value,			/* value to reset for string2 */
               &s,			/* input pointer */
               &yield,			/* output pointer */
               US"if",			/* condition type */
	       &resetok))
        {
        case 1: goto EXPAND_FAILED;          /* when all is well, the */
        case 2: goto EXPAND_FAILED_CURLY;    /* returned value is 0 */
        }

      /* Restore external setting of expansion variables for continuation
      at this level. */

      lookup_value = save_lookup_value;
      restore_expand_strings(save_expand_nmax, save_expand_nstring,
        save_expand_nlength);
      break;
      }

#ifdef SUPPORT_I18N
    case EITEM_IMAPFOLDER:
      {				/* ${imapfolder {name}{sep}{specials}} */
      uschar * sub_arg[3];
      const uschar * encoded;

      switch(read_subs(sub_arg, nelem(sub_arg), 1, &s, flags, TRUE, name, &resetok, NULL))
        {
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      if (!sub_arg[1])			/* One argument */
	{
	sub_arg[1] = US"/";		/* default separator */
	sub_arg[2] = NULL;
	}
      else if (Ustrlen(sub_arg[1]) != 1)
	{
	expand_string_message =
	  string_sprintf(
		"IMAP folder separator must be one character, found %q",
		sub_arg[1]);
	goto EXPAND_FAILED;
	}

      if (flags & ESI_SKIPPING) continue;

      if (!(encoded = imap_utf7_encode(sub_arg[0], headers_charset,
			  sub_arg[1][0], sub_arg[2], &expand_string_message)))
	goto EXPAND_FAILED;
      yield = string_cat(yield, encoded);
      break;
      }
#endif

    /* Handle database lookups unless locked out. If "skipping" is TRUE, we are
    expanding an internal string that isn't actually going to be used. All we
    need to do is check the syntax, so don't do a lookup at all. Preserve the
    values of the numerical expansion variables in case they get changed by a
    partial lookup. If not, they retain their external settings. At the end
    of this "lookup" section, they get restored to their previous values. */

    case EITEM_LOOKUP:
      {
      int expand_setup = 0, nameptr = 0;
      int partial, affixlen, starflags;
      const lookup_info * li;
      const uschar * key, * affix, * opts;
      uschar * save_lookup_value = lookup_value, * filename;
      int save_expand_nmax =
        save_expand_strings(save_expand_nstring, save_expand_nlength);

      if (expand_forbid & RDO_LOOKUP)
        {
        expand_string_message = US"lookup expansions are not permitted";
        goto EXPAND_FAILED;
        }

      /* Get the key we are to look up for single-key+file style lookups.
      Otherwise set the key NULL pro-tem. */

      if (Uskip_whitespace(&s) == '{')					/*}*/
        {
        key = expand_string_internal(s+1,
		ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL);
        if (!key) goto EXPAND_FAILED;			/*{{*/
        if (*s++ != '}')
	  {
	  expand_string_message = US"missing '}' after lookup key";
	  goto EXPAND_FAILED_CURLY;
	  }
        Uskip_whitespace(&s);
        }
      else key = NULL;

      /* Find out the type of database */

      if (!isalpha(*s))
        {
        expand_string_message = US"missing lookup type";
        goto EXPAND_FAILED;
        }

      /* The type is a string that may contain special characters of various
      kinds. Allow everything except space or { to appear; the actual content
      is checked by search_findtype_partial. */		/*}*/

      while (*s && *s != '{' && !isspace(*s))		/*}*/
        {
        if (nameptr < sizeof(name) - 1) name[nameptr++] = *s;
        s++;
        }
      name[nameptr] = '\0';
      Uskip_whitespace(&s);

      /* Now check for the individual search type and any partial or default
      options. Only those types that are actually in the binary are valid. */

      if (!(li = search_findtype_partial(name, &partial, &affix, &affixlen,
	  &starflags, &opts)))
        {
        expand_string_message = search_error_message;
        goto EXPAND_FAILED;
        }

      /* Check that a key was provided for those lookup types that need it,
      and was not supplied for those that use the query style. */

      if (!mac_islookup(li, lookup_querystyle|lookup_absfilequery))
        {
        if (!key)
          {
          expand_string_message = string_sprintf("missing {key} for single-"
            "key %q lookup", name);
          goto EXPAND_FAILED;
          }
        }
      else if (key)
	{
	expand_string_message = string_sprintf("a single key was given for "
	  "lookup type %q, which is not a single-key lookup type", name);
	goto EXPAND_FAILED;
	}

      /* Get the next string in brackets and expand it. It is the file name for
      single-key+file lookups, and the whole query otherwise. In the case of
      queries that also require a file name (e.g. sqlite), the file name comes
      first. */

      if (*s != '{')
        {
	expand_string_message = US"missing '{' for lookup file-or-query arg";
	goto EXPAND_FAILED_CURLY;						/*}}*/
	}
      if (!(filename = expand_string_internal(s+1,
		ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL)))
	goto EXPAND_FAILED;
      										/*{{*/
      if (*s++ != '}')
        {
	expand_string_message = US"missing '}' closing lookup file-or-query arg";
	goto EXPAND_FAILED_CURLY;
	}
      Uskip_whitespace(&s);

      /* If this isn't a single-key+file lookup, re-arrange the variables
      to be appropriate for the search_ functions. For query-style lookups,
      there is just a "key", and no file name. For the special query-style +
      file types, the query (i.e. "key") starts with a file name. */

      if (!key)
	key = search_args(li, name, filename, &filename, opts);

      /* If skipping, don't do the next bit - just lookup_value == NULL, as if
      the entry was not found. Note that there is no search_close() function.
      Files are left open in case of re-use. At suitable places in higher logic,
      search_tidyup() is called to tidy all open files. This can save opening
      the same file several times. However, files may also get closed when
      others are opened, if too many are open at once. The rule is that a
      handle should not be used after a second search_open().

      Request that a partial search sets up $1 and maybe $2 by passing
      expand_setup containing zero. If its value changes, reset expand_nmax,
      since new variables will have been set. Note that at the end of this
      "lookup" section, the old numeric variables are restored. */

      if (flags & ESI_SKIPPING)
        lookup_value = NULL;
      else
        {
        void * handle = search_open(filename, li, 0, NULL, NULL);
        if (!handle)
          {
          expand_string_message = search_error_message;
          goto EXPAND_FAILED;
          }
        lookup_value = search_find(handle, filename, key, partial, affix,
          affixlen, starflags, &expand_setup, opts);
        if (f.search_find_defer)
          {
          expand_string_message = string_sprintf("lookup of %q gave DEFER: %q",
						key, search_error_message);
          goto EXPAND_FAILED;
          }
        if (expand_setup > 0) expand_nmax = expand_setup;
        }

      /* The handling of "yes" and "no" result strings is now in a separate
      function that is also used by ${if} and ${extract}. */

      switch(process_yesno(
               flags,			/* were previously skipping */
               lookup_value != NULL,	/* success/failure indicator */
               save_lookup_value,	/* value to reset for string2 */
               &s,			/* input pointer */
               &yield,			/* output pointer */
               US"lookup",		/* condition type */
	       &resetok))
        {
        case 1: goto EXPAND_FAILED;          /* when all is well, the */
        case 2: goto EXPAND_FAILED_CURLY;    /* returned value is 0 */
        }

      /* Restore external setting of expansion variables for carrying on
      at this level, and continue. */

      restore_expand_strings(save_expand_nmax, save_expand_nstring,
        save_expand_nlength);

      if (flags & ESI_SKIPPING) continue; else break;
      }

    /* If Perl support is configured, handle calling embedded perl subroutines,
    unless locked out at this time. Syntax is ${perl{sub}} or ${perl{sub}{arg}}
    or ${perl{sub}{arg1}{arg2}} or up to a maximum of EXIM_PERL_MAX_ARGS
    arguments (defined below). */

#define EXIM_PERL_MAX_ARGS 8

    case EITEM_PERL:
#ifndef EXIM_PERL
      expand_string_message = US"\"${perl\" encountered, but this facility "	/*}*/
	"is not included in this binary";
      goto EXPAND_FAILED;

#else   /* EXIM_PERL */
      {
      uschar * sub_arg[EXIM_PERL_MAX_ARGS + 2];
      gstring * new_yield;
      const misc_module_info * mi;

      if (expand_forbid & RDO_PERL)
        {
        expand_string_message = US"Perl calls are not permitted";
        goto EXPAND_FAILED;
        }

      switch(read_subs(sub_arg, EXIM_PERL_MAX_ARGS + 1, 1, &s, flags, TRUE,
           name, &resetok, NULL))
        {
	case -1: continue;	/* If skipping, we don't actually do anything */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      /* Start the interpreter if necessary */

      if (!(mi = perl_startup(opt_perl_startup)))
	goto EXPAND_FAILED;

      /* Call the function */

      sub_arg[EXIM_PERL_MAX_ARGS + 1] = NULL;
	{
	typedef gstring * (*fn_t)
			    (gstring *, uschar **, uschar *, const uschar **);
	new_yield = (((fn_t *) mi->functions)[PERL_CAT])
					      (yield, &expand_string_message,
						sub_arg[0], CUSS sub_arg + 1);
	}

      /* NULL yield indicates failure; if the message pointer has been set to
      NULL, the yield was undef, indicating a forced failure. Otherwise the
      message will indicate some kind of Perl error. */

      if (!new_yield)
        {
        if (!expand_string_message)
          {
          expand_string_message =
            string_sprintf("Perl subroutine %q returned undef to force "
              "failure", sub_arg[0]);
          f.expand_string_forcedfail = TRUE;
          }
        goto EXPAND_FAILED;
        }

      /* Yield succeeded. Ensure forcedfail is unset, just in case it got
      set during a callback from Perl. */

      f.expand_string_forcedfail = FALSE;
      yield = new_yield;
      break;
      }
#endif /* EXIM_PERL */

    /* Transform email address to "prvs" scheme to use
       as BATV-signed return path */

    case EITEM_PRVS:
      {
      uschar * sub_arg[3], * domain;
      const uschar * p;

      switch(read_subs(sub_arg, 3, 2, &s, flags, TRUE, name, &resetok, NULL))
        {
	case -1: continue;	/* If skipping, we don't actually do anything */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      /* sub_arg[0] is the address */
      if (  !(domain = Ustrrchr(sub_arg[0],'@'))
	 || domain == sub_arg[0] || Ustrlen(domain) == 1)
        {
        expand_string_message = US"prvs first argument must be a qualified email address";
        goto EXPAND_FAILED;
        }

      /* Calculate the hash. The third argument must be a single-digit
      key number, or unset. */

      if (  sub_arg[2]
         && (!isdigit(sub_arg[2][0]) || sub_arg[2][1] != 0))
        {
        expand_string_message = US"prvs third argument must be a single digit";
        goto EXPAND_FAILED;
        }

      p = prvs_hmac_sha1(sub_arg[0], sub_arg[1], sub_arg[2], prvs_daystamp(7));
      if (!p)
        {
        expand_string_message = US"prvs hmac-sha1 conversion failed";
        goto EXPAND_FAILED;
        }

      /* Now separate the domain from the local part */
      *domain++ = '\0';

      yield = string_catn(yield, US"prvs=", 5);
      yield = string_catn(yield, sub_arg[2] ? sub_arg[2] : US"0", 1);
      yield = string_catn(yield, prvs_daystamp(7), 3);
      yield = string_catn(yield, p, 6);
      yield = string_catn(yield, US"=", 1);
      yield = string_cat (yield, sub_arg[0]);
      yield = string_catn(yield, US"@", 1);
      yield = string_cat (yield, domain);

      break;
      }

    /* Check a prvs-encoded address for validity */

    case EITEM_PRVSCHECK:
      {
      uschar * sub_arg[3], * p;
      gstring * g;
      const pcre2_code * re;

      /* Reset expansion variables */
      prvscheck_result = NULL;
      prvscheck_address = NULL;
      prvscheck_keynum = NULL;

      switch(read_subs(sub_arg, 1, 1, &s, flags, FALSE, name, &resetok, NULL))
        {
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      re = regex_must_compile(
	US"^prvs\\=([0-9])([0-9]{3})([A-F0-9]{6})\\=(.+)\\@(.+)$",
	MCS_CASELESS | MCS_CACHEABLE, FALSE);

      if (regex_match_and_setup(re,sub_arg[0],0,-1))
        {
        uschar * local_part = string_copyn(expand_nstring[4],expand_nlength[4]);
        uschar * key_num = string_copyn(expand_nstring[1],expand_nlength[1]);
        uschar * daystamp = string_copyn(expand_nstring[2],expand_nlength[2]);
        uschar * hash = string_copyn(expand_nstring[3],expand_nlength[3]);
        uschar * domain = string_copyn(expand_nstring[5],expand_nlength[5]);

        DEBUG(D_expand)
	  {
	  debug_printf_indent("prvscheck localpart: %s\n", local_part);
	  debug_printf_indent("prvscheck key number: %s\n", key_num);
	  debug_printf_indent("prvscheck daystamp: %s\n", daystamp);
	  debug_printf_indent("prvscheck hash: %s\n", hash);
	  debug_printf_indent("prvscheck domain: %s\n", domain);
	  }

        /* Set up expansion variables */
        g = string_cat (NULL, local_part);
        g = string_catn(g, US"@", 1);
        g = string_cat (g, domain);
        prvscheck_address = string_from_gstring(g);
        prvscheck_keynum = string_copy(key_num);

        /* Now expand the second argument */
        switch(read_subs(sub_arg, 1, 1, &s, flags, FALSE, name, &resetok, NULL))
          {
          case 1: goto EXPAND_FAILED_CURLY;
          case 2:
          case 3: goto EXPAND_FAILED;
          }

        /* Now we have the key and can check the address. */

        p = prvs_hmac_sha1(prvscheck_address, sub_arg[0], prvscheck_keynum,
          daystamp);
        if (!p)
          {
          expand_string_message = US"hmac-sha1 conversion failed";
          goto EXPAND_FAILED;
          }

        DEBUG(D_expand) debug_printf_indent("prvscheck: received hash is %s\n", hash);
        DEBUG(D_expand) debug_printf_indent("prvscheck:      own hash is %s\n", p);

        if (Ustrcmp(p,hash) == 0)
          {
          /* Success, valid BATV address. Now check the expiry date. */
          const uschar * now = prvs_daystamp(0);
          unsigned int inow = 0, iexpire = 1;

          (void)sscanf(CS now,"%u",&inow);
          (void)sscanf(CS daystamp,"%u",&iexpire);

          /* When "iexpire" is < 7, a "flip" has occurred.
             Adjust "inow" accordingly. */
          if ( (iexpire < 7) && (inow >= 993) ) inow = 0;

          if (iexpire >= inow)
            {
            prvscheck_result = US"1";
            DEBUG(D_expand) debug_printf_indent("prvscheck: success, $prvscheck_result set to 1\n");
            }
	  else
            {
            prvscheck_result = NULL;
            DEBUG(D_expand) debug_printf_indent("prvscheck: signature expired, $prvscheck_result unset\n");
            }
          }
        else
          {
          prvscheck_result = NULL;
          DEBUG(D_expand) debug_printf_indent("prvscheck: hash failure, $prvscheck_result unset\n");
          }

        /* Now expand the final argument. We leave this till now so that
        it can include $prvscheck_result. */

        switch(read_subs(sub_arg, 1, 0, &s, flags, TRUE, name, &resetok, NULL))
          {
          case 1: goto EXPAND_FAILED_CURLY;
          case 2:
          case 3: goto EXPAND_FAILED;
          }

	yield = string_cat(yield,
	  !sub_arg[0] || !*sub_arg[0] ? prvscheck_address : sub_arg[0]);

        /* Reset the "internal" variables afterwards, because they are in
        dynamic store that will be reclaimed if the expansion succeeded. */

        prvscheck_address = NULL;
        prvscheck_keynum = NULL;
        }
      else
        /* Does not look like a prvs encoded address, return the empty string.
           We need to make sure all subs are expanded first, so as to skip over
           the entire item. */

        switch(read_subs(sub_arg, 2, 1, &s, flags, TRUE, name, &resetok, NULL))
          {
          case 1: goto EXPAND_FAILED_CURLY;
          case 2:
          case 3: goto EXPAND_FAILED;
          }

      if (flags & ESI_SKIPPING) continue;
      break;
      }

    /* Handle "readfile" to insert an entire file */

    case EITEM_READFILE:
      {
      FILE * f;
      uschar * sub_arg[2];

      if (expand_forbid & RDO_READFILE)
        {
        expand_string_message = US"file insertions are not permitted";
        goto EXPAND_FAILED;
        }

      switch(read_subs(sub_arg, 2, 1, &s, flags, TRUE, name, &resetok, NULL))
        {
	case -1: continue;	/* If skipping, we don't actually do anything */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      /* Open the file and read it */

      if (!(f = Ufopen(sub_arg[0], "rb")))
        {
        expand_string_message = string_open_failed("%s", sub_arg[0]);
        goto EXPAND_FAILED;
        }

      yield = cat_file(f, yield, sub_arg[1]);
      (void)fclose(f);
      break;
      }

    /* Handle "readsocket" to insert data from a socket, either
    Inet or Unix domain */

    case EITEM_READSOCK:
      {
      const uschar * arg;
      uschar * sub_arg[4];

      if (expand_forbid & RDO_READSOCK)
        {
        expand_string_message = US"socket insertions are not permitted";
        goto EXPAND_FAILED;
        }

      /* Read up to 4 arguments, but don't do the end of item check afterwards,
      because there may be a string for expansion on failure. */

      switch(read_subs(sub_arg, 4, 2, &s, flags, FALSE, name, &resetok, NULL))
        {
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:                             /* Won't occur: no end check */
        case 3: goto EXPAND_FAILED;
        }

      /* If skipping, we don't actually do anything. Otherwise, arrange to
      connect to either an IP or a Unix socket. */

      if (!(flags & ESI_SKIPPING))
        {
	const lookup_info * li = search_findtype(US"readsock", 8);
	gstring * g = NULL;
	void * handle;
	int expand_setup = -1;
	const uschar * t;

	if (!li)
	  {
	  expand_string_message = search_error_message;
	  goto EXPAND_FAILED;
	  }

	/* If the reqstr is empty, flag that and set a dummy */

	if (!sub_arg[1][0])
	  {
	  g = string_append_listele(g, ',', US"send=no");
	  sub_arg[1] = US"DUMMY";
	  }

	/* Re-marshall the options */

	if (sub_arg[2])
	  {
	  const uschar * list = sub_arg[2];
	  uschar * item;
	  int sep = 0;

	  /* First option has no tag and is timeout */
	  if ((item = string_nextinlist(&list, &sep, NULL, 0)))
	    g = string_append_listele_fmt(g, ',', TRUE, "timeout=%s", item);

	  /* The rest of the options from the expansion */
	  while ((item = string_nextinlist(&list, &sep, NULL, 0)))
	    g = string_append_listele(g, ',', item);

	  /* possibly plus an EOL string.  Process with escapes, to protect
	  from list-processing.  The only current user of eol= in search
	  options is the readsock expansion. */

	  if (sub_arg[3] && *sub_arg[3])
	    g = string_append_listele_fmt(g, ',', TRUE, 
		  "eol=%s", string_printing2(sub_arg[3], SP_TAB|SP_SPACE));
	  }

	/* Gat a (possibly cached) handle for the connection */

	if (!(handle = search_open(sub_arg[0], li, 0, NULL, NULL)))
	  {
	  if (*expand_string_message) goto EXPAND_FAILED;
	  expand_string_message = search_error_message;
	  search_error_message = NULL;
	  goto SOCK_FAIL;
	  }

	/* Get (possibly cached) results for the lookup */
	/* sspec: sub_arg[0]  req: sub_arg[1]  opts: g */

	if ((t = search_find(handle, sub_arg[0], sub_arg[1], -1, NULL, 0, 0,
				    &expand_setup, string_from_gstring(g))))
	  yield = string_cat(yield, t);
	else if (f.search_find_defer)
	  {
	  expand_string_message = search_error_message;
	  search_error_message = NULL;
	  goto SOCK_FAIL;
	  }
	else
	  {	/* should not happen, at present */
	  expand_string_message = search_error_message;
	  search_error_message = NULL;
	  goto SOCK_FAIL;
	  }
        }

      /* The whole thing has worked (or we were skipping). If there is a
      failure string following, we need to skip it. */

      if (*s == '{')							/*}*/
        {
        if (!expand_string_internal(s+1,
	  ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | ESI_SKIPPING, &s, &resetok, NULL))
          goto EXPAND_FAILED;						/*{*/
        if (*s++ != '}')
	  {								/*{*/
	  expand_string_message = US"missing '}' closing failstring for readsocket";
	  goto EXPAND_FAILED_CURLY;
	  }
        Uskip_whitespace(&s);
        }

    READSOCK_DONE:							/*{*/
      if (*s++ != '}')
        {								/*{*/
	expand_string_message = US"missing '}' closing readsocket";
	goto EXPAND_FAILED_CURLY;
	}
      if (flags & ESI_SKIPPING) continue; else break;

      /* Come here on failure to create socket, connect socket, write to the
      socket, or timeout on reading. If another substring follows, expand and
      use it. Otherwise, those conditions give expand errors. */

    SOCK_FAIL:
      if (*s != '{') goto EXPAND_FAILED;				/*}*/
      DEBUG(D_any) debug_printf("%s\n", expand_string_message);
      if (!(arg = expand_string_internal(s+1,
		    ESI_BRACE_ENDS | ESI_HONOR_DOLLAR, &s, &resetok, NULL)))
        goto EXPAND_FAILED;
      yield = string_cat(yield, arg);					/*{*/
      if (*s++ != '}')
        {								/*{*/
	expand_string_message = US"missing '}' closing failstring for readsocket";
	goto EXPAND_FAILED_CURLY;
	}
      Uskip_whitespace(&s);
      goto READSOCK_DONE;
      }

    /* Handle "run" to execute a program. */

    case EITEM_RUN:
      {
      FILE * f;
      const uschar * arg, ** argv;
      unsigned late_expand = TSUC_EXPAND_ARGS | TSUC_ALLOW_TAINTED_ARGS | TSUC_ALLOW_RECIPIENTS;

      if (expand_forbid & RDO_RUN)
        {
        expand_string_message = US"running a command is not permitted";
        goto EXPAND_FAILED;
        }

      /* Handle options to the "run" */

      while (*s == ',')
	if (Ustrncmp(++s, "preexpand", 9) == 0)
	  { late_expand = 0; s += 9; }
	else
	  {
	  const uschar * t = s;
	  while (isalpha(*++t)) ;
	  expand_string_message = string_sprintf("bad option '%.*s' for run",
						  (int)(t-s), s);
	  goto EXPAND_FAILED;
	  }
      Uskip_whitespace(&s);

      if (*s != '{')					/*}*/
        {
	expand_string_message = US"missing '{' for command arg of run";
	goto EXPAND_FAILED_CURLY;			/*"}*/
	}
      s++;

      if (late_expand)		/* this is the default case */
	{
	int n;
	const uschar * t;
	/* Locate the end of the args */
	(void) expand_string_internal(s,
	  ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | ESI_SKIPPING, &t, NULL, NULL);
	n = t - s;
	arg = flags & ESI_SKIPPING ? NULL : string_copyn(s, n);
	s += n;
	}
      else
	{
	DEBUG(D_expand)
	  debug_printf_indent("args string for ${run} expand before split\n");
	if (!(arg = expand_string_internal(s,
		ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL)))
	  goto EXPAND_FAILED;
	Uskip_whitespace(&s);
	}
							/*{*/
      if (*s++ != '}')
        {						/*{*/
	expand_string_message = US"missing '}' closing command arg of run";
	goto EXPAND_FAILED_CURLY;
	}

      if (flags & ESI_SKIPPING)   /* Just pretend it worked when we're skipping */
	{
        runrc = 0;
	lookup_value = NULL;
	}
      else
        {
	int fd_in, fd_out;
	pid_t pid;

        if (!transport_set_up_command(&argv,    /* anchor for arg list */
            arg,                                /* raw command */
	    late_expand,		/* expand args if not already done */
            0,                          /* not relevant when... */
            NULL,                       /* no transporting address */
            US"${run} expansion",       /* for error messages */
            &expand_string_message))    /* where to put error message */
          goto EXPAND_FAILED;

        /* Create the child process, making it a group leader. */

        if ((pid = child_open(USS argv, NULL, 0077, &fd_in, &fd_out, TRUE,
			      US"expand-run")) < 0)
          {
          expand_string_message =
            string_sprintf("couldn't create child process: %s", strerror(errno));
          goto EXPAND_FAILED;
	  }

        /* Nothing is written to the standard input. */

        (void)close(fd_in);

        /* Read the pipe to get the command's output into $value (which is kept
        in lookup_value). Read during execution, so that if the output exceeds
        the OS pipe buffer limit, we don't block forever. Remember to not release
	memory just allocated for $value. */

	resetok = FALSE;
        f = fdopen(fd_out, "rb");
        sigalrm_seen = FALSE;
        ALARM(60);
	lookup_value = string_from_gstring(cat_file(f, NULL, NULL));
        ALARM_CLR(0);
        (void)fclose(f);

        /* Wait for the process to finish, applying the timeout, and inspect its
        return code for serious disasters. Simple non-zero returns are passed on.
        */

        if (sigalrm_seen || (runrc = child_close(pid, 30)) < 0)
          {
          if (sigalrm_seen || runrc == -256)
            {
            expand_string_message = US"command timed out";
            killpg(pid, SIGKILL);       /* Kill the whole process group */
            }

          else if (runrc == -257)
            expand_string_message = string_sprintf("wait() failed: %s",
              strerror(errno));

          else
            expand_string_message = string_sprintf("command killed by signal %d",
              -runrc);

          goto EXPAND_FAILED;
          }
        }

      /* Process the yes/no strings; $value may be useful in both cases */

      switch(process_yesno(
               flags,			/* were previously skipping */
               runrc == 0,		/* success/failure indicator */
               lookup_value,		/* value to reset for string2 */
               &s,			/* input pointer */
               &yield,			/* output pointer */
               US"run",			/* condition type */
	       &resetok))
        {
        case 1: goto EXPAND_FAILED;          /* when all is well, the */
        case 2: goto EXPAND_FAILED_CURLY;    /* returned value is 0 */
        }

      if (flags & ESI_SKIPPING) continue; else break;
      }

    /* Handle character translation for "tr" */

    case EITEM_TR:
      {
      int oldptr = gstring_length(yield);
      int o2m;
      uschar * sub[3];

      switch(read_subs(sub, 3, 3, &s, flags, TRUE, name, &resetok, NULL))
        {
	case -1: continue;	/* skipping */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      if (  (yield = string_cat(yield, sub[0]))
         && (o2m = Ustrlen(sub[2]) - 1) >= 0)
	  for (; oldptr < yield->ptr; oldptr++)
        {
        const uschar * m = Ustrrchr(sub[1], yield->s[oldptr]);
        if (m)
          {
          int o = m - sub[1];
          yield->s[oldptr] = sub[2][o < o2m ? o : o2m];
          }
        }

      break;
      }

    /* Handle "hash", "length", "nhash", and "substr" when they are given with
    expanded arguments. */

    case EITEM_HASH:
    case EITEM_LENGTH:
    case EITEM_NHASH:
    case EITEM_SUBSTR:
      {
      int len;
      uschar *ret;
      int val[2] = { 0, -1 };
      uschar * sub[3];

      /* "length" takes only 2 arguments whereas the others take 2 or 3.
      Ensure that sub[2] is set in the ${length } case. */

      sub[2] = NULL;
      switch(read_subs(sub, item_type == EITEM_LENGTH ? 2:3, 2, &s, flags,
             TRUE, name, &resetok, NULL))
        {
	case -1: continue;	/* skipping */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      /* Juggle the arguments if there are only two of them: always move the
      string to the last position and make ${length{n}{str}} equivalent to
      ${substr{0}{n}{str}}. See the defaults for val[] above. */

      if (!sub[2])
        {
        sub[2] = sub[1];
        sub[1] = NULL;
        if (item_type == EITEM_LENGTH)
          {
          sub[1] = sub[0];
          sub[0] = NULL;
          }
        }

      for (int i = 0; i < 2; i++) if (sub[i])
        {
	if (is_tainted(sub[i]))
	  {
	  expand_string_message =
	    string_sprintf("attempt to use tainted string '%s' for %s",
			sub[i], name);
	  log_write(0, LOG_MAIN|LOG_PANIC, "%s", expand_string_message);
	  goto EXPAND_FAILED;
	  }
        val[i] = (int)Ustrtol(sub[i], &ret, 10);
        if (*ret != 0  ||  i != 0 && val[i] < 0)
          {
          expand_string_message = string_sprintf("%q is not a%s number "
            "(in %q expansion)", sub[i], i != 0 ? " positive" : "", name);
          goto EXPAND_FAILED;
          }
        }

      ret =
        item_type == EITEM_HASH
	?  compute_hash(sub[2], val[0], val[1], &len)
	: item_type == EITEM_NHASH
	? compute_nhash(sub[2], val[0], val[1], &len)
	: extract_substr(sub[2], val[0], val[1], &len);
      if (!ret)
	goto EXPAND_FAILED;
      yield = string_catn(yield, ret, len);
      break;
      }

    /* Handle HMAC computation: ${hmac{<algorithm>}{<secret>}{<text>}}
    This code originally contributed by Steve Haslam. It currently supports
    the use of MD5 and SHA-1 hashes.

    We need some workspace that is large enough to handle all the supported
    hash types. Use macros to set the sizes rather than be too elaborate. */

    #define MAX_HASHLEN      20
    #define MAX_HASHBLOCKLEN 64

    case EITEM_HMAC:
      {
      uschar * sub[3];
      md5 md5_base;
      hctx sha1_ctx;
      void * use_base;
      int type;
      int hashlen;      /* Number of octets for the hash algorithm's output */
      int hashblocklen; /* Number of octets the hash algorithm processes */
      uschar * keyptr, * p;
      unsigned int keylen;

      uschar keyhash[MAX_HASHLEN];
      uschar innerhash[MAX_HASHLEN];
      uschar finalhash[MAX_HASHLEN];
      uschar finalhash_hex[2*MAX_HASHLEN];
      uschar innerkey[MAX_HASHBLOCKLEN];
      uschar outerkey[MAX_HASHBLOCKLEN];

      switch (read_subs(sub, 3, 3, &s, flags, TRUE, name, &resetok, NULL))
        {
	case -1: continue;	/* skipping */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      if (Ustrcmp(sub[0], "md5") == 0)
	{
	type = HMAC_MD5;
	use_base = &md5_base;
	hashlen = 16;
	hashblocklen = 64;
	}
      else if (Ustrcmp(sub[0], "sha1") == 0)
	{
	type = HMAC_SHA1;
	use_base = &sha1_ctx;
	hashlen = 20;
	hashblocklen = 64;
	}
      else
	{
	expand_string_message =
	  string_sprintf("hmac algorithm %q is not recognised", sub[0]);
	goto EXPAND_FAILED;
	}

      keyptr = sub[1];
      keylen = Ustrlen(keyptr);

      /* If the key is longer than the hash block length, then hash the key
      first */

      if (keylen > hashblocklen)
	{
	chash_start(type, use_base);
	chash_end(type, use_base, keyptr, keylen, keyhash);
	keyptr = keyhash;
	keylen = hashlen;
	}

      /* Now make the inner and outer key values */

      memset(innerkey, 0x36, hashblocklen);
      memset(outerkey, 0x5c, hashblocklen);

      for (int i = 0; i < keylen; i++)
	{
	innerkey[i] ^= keyptr[i];
	outerkey[i] ^= keyptr[i];
	}

      /* Now do the hashes */

      chash_start(type, use_base);
      chash_mid(type, use_base, innerkey);
      chash_end(type, use_base, sub[2], Ustrlen(sub[2]), innerhash);

      chash_start(type, use_base);
      chash_mid(type, use_base, outerkey);
      chash_end(type, use_base, innerhash, hashlen, finalhash);

      /* Encode the final hash as a hex string */

      p = finalhash_hex;
      for (int i = 0; i < hashlen; i++)
	{
	*p++ = hex_digits[(finalhash[i] & 0xf0) >> 4];
	*p++ = hex_digits[finalhash[i] & 0x0f];
	}

      DEBUG(D_any) debug_printf("HMAC[%s](%.*s,%s)=%.*s\n",
	sub[0], (int)keylen, keyptr, sub[2], hashlen*2, finalhash_hex);

      yield = string_catn(yield, finalhash_hex, hashlen*2);
      break;
      }

    /* Handle global substitution for "sg" - like Perl's s/xxx/yyy/g operator.
    We have to save the numerical variables and restore them afterwards. */

    case EITEM_SG:
      {
      const pcre2_code * re;
      int moffset, moffsetextra, slen;
      pcre2_match_data * md;
      int emptyopt;
      uschar * subject, * sub[3];
      int save_expand_nmax =
        save_expand_strings(save_expand_nstring, save_expand_nlength);
      unsigned sub_textonly = 0;

      switch(read_subs(sub, 3, 3, &s, flags, TRUE, name, &resetok, &sub_textonly))
        {
	case -1: continue;	/* skipping */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      /* Compile the regular expression */

      re = regex_compile(sub[1],
	      sub_textonly & BIT(1) ? MCS_CACHEABLE : MCS_NOFLAGS,
	      &expand_string_message, pcre_gen_cmp_ctx);
      if (!re)
        goto EXPAND_FAILED;

      md = pcre2_match_data_create(EXPAND_MAXN + 1, pcre_gen_ctx);

      /* Now run a loop to do the substitutions as often as necessary. It ends
      when there are no more matches. Take care over matches of the null string;
      do the same thing as Perl does. */

      subject = sub[0];
      slen = Ustrlen(sub[0]);
      moffset = moffsetextra = 0;
      emptyopt = 0;

      for (;;)
        {
	PCRE2_SIZE * ovec = pcre2_get_ovector_pointer(md);
	int n = pcre2_match(re, (PCRE2_SPTR)subject, slen, moffset + moffsetextra,
	  PCRE_EOPT | emptyopt, md, pcre_gen_mtc_ctx);
        const uschar * insert;

        /* No match - if we previously set PCRE_NOTEMPTY after a null match, this
        is not necessarily the end. We want to repeat the match from one
        character further along, but leaving the basic offset the same (for
        copying below). We can't be at the end of the string - that was checked
        before setting PCRE_NOTEMPTY. If PCRE_NOTEMPTY is not set, we are
        finished; copy the remaining string and end the loop. */

        if (n < 0)
          {
          if (emptyopt != 0)
            {
            moffsetextra = 1;
            emptyopt = 0;
            continue;
            }
          yield = string_catn(yield, subject+moffset, slen-moffset);
          break;
          }

        /* Match - set up for expanding the replacement. */
	DEBUG(D_expand) debug_printf_indent("%s: match\n", name);

        if (n == 0) n = EXPAND_MAXN + 1;
        expand_nmax = 0;
        for (int nn = 0; nn < n*2; nn += 2)
          {
          expand_nstring[expand_nmax] = subject + ovec[nn];
          expand_nlength[expand_nmax++] = ovec[nn+1] - ovec[nn];
          }
        expand_nmax--;

        /* Copy the characters before the match, plus the expanded insertion. */

	yield = string_catn(yield, subject + moffset, ovec[0] - moffset);

        if (!(insert = expand_string(sub[2])))
	  goto EXPAND_FAILED;
        yield = string_cat(yield, insert);

        moffset = ovec[1];
        moffsetextra = 0;
        emptyopt = 0;

        /* If we have matched an empty string, first check to see if we are at
        the end of the subject. If so, the loop is over. Otherwise, mimic
        what Perl's /g options does. This turns out to be rather cunning. First
        we set PCRE_NOTEMPTY and PCRE_ANCHORED and try the match a non-empty
        string at the same point. If this fails (picked up above) we advance to
        the next character. */

        if (ovec[0] == ovec[1])
          {
          if (ovec[0] == slen) break;
          emptyopt = PCRE2_NOTEMPTY | PCRE2_ANCHORED;
          }
        }

      /* All done - restore numerical variables. */

      /* pcre2_match_data_free(md);	gen ctx needs no free */
      restore_expand_strings(save_expand_nmax, save_expand_nstring,
        save_expand_nlength);
      break;
      }

    /* Handle keyed and numbered substring extraction. If the first argument
    consists entirely of digits, then a numerical extraction is assumed. */

    case EITEM_EXTRACT:
      {
      int field_number = 1;
      BOOL field_number_set = FALSE;
      uschar * save_lookup_value = lookup_value, * sub[3];
      int save_expand_nmax =
        save_expand_strings(save_expand_nstring, save_expand_nlength);

      /* On reflection the original behaviour of extract-json for a string
      result, leaving it quoted, was a mistake.  But it was already published,
      hence the addition of jsons.  In a future major version, make json
      work like josons, and withdraw jsons. */

      enum {extract_basic, extract_json, extract_jsons} fmt = extract_basic;

      /* Check for a format-variant specifier */

      if (Uskip_whitespace(&s) != '{')					/*}*/
	if (Ustrncmp(s, "json", 4) == 0)
	  if (*(s += 4) == 's')
	    {fmt = extract_jsons; s++;}
	  else
	    fmt = extract_json;

      /* While skipping we cannot rely on the data for expansions being
      available (eg. $item) hence cannot decide on numeric vs. keyed.
      Read a maximum of 5 arguments (including the yes/no) */

      if (flags & ESI_SKIPPING)
	{
        for (int j = 5; j > 0 && *s == '{'; j--)			/*'}'*/
	  {
          if (!expand_string_internal(s+1,
		ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL))
	    goto EXPAND_FAILED;					/*'{'*/
          if (*s++ != '}')
	    {
	    expand_string_message = US"missing '{' for arg of extract";
	    goto EXPAND_FAILED_CURLY;
	    }
	  Uskip_whitespace(&s);
	  }
	if (  Ustrncmp(s, "fail", 4) == 0				/*'{'*/
	   && (s[4] == '}' || s[4] == ' ' || s[4] == '\t' || !s[4])
	   )
	  {
	  s += 4;
	  Uskip_whitespace(&s);
	  }								/*'{'*/
	if (*s != '}')
	  {
	  expand_string_message = US"missing '}' closing extract";
	  goto EXPAND_FAILED_CURLY;
	  }
	}

      else for (int i = 0, j = 2; i < j; i++) /* Read the proper number of arguments */
        {
	if (Uskip_whitespace(&s) == '{') 				/*'}'*/
          {
          if (!(sub[i] = expand_string_internal(s+1,
		ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL)))
	    goto EXPAND_FAILED;						/*'{'*/
          if (*s++ != '}')
	    {
	    expand_string_message = string_sprintf(
	      "missing '}' closing arg %d of extract", i+1);
	    goto EXPAND_FAILED_CURLY;
	    }

          /* After removal of leading and trailing white space, the first
          argument must not be empty; if it consists entirely of digits
          (optionally preceded by a minus sign), this is a numerical
          extraction, and we expect 3 arguments (normal) or 2 (json). */

          if (i == 0)
            {
            int len;
            int x = 0;
            uschar * p = sub[0];

            Uskip_whitespace(&p);
            sub[0] = p;

            len = Ustrlen(p);
            while (len > 0 && isspace(p[len-1])) len--;
            p[len] = 0;

	    if (!*p)
	      {
	      expand_string_message = US"first argument of \"extract\" must "
		"not be empty";
	      goto EXPAND_FAILED;
	      }

	    if (*p == '-')
	      {
	      field_number = -1;
	      p++;
	      }
	    while (*p && isdigit(*p)) x = x * 10 + *p++ - '0';
	    if (!*p)
	      {
	      field_number *= x;
	      if (fmt == extract_basic) j = 3;               /* Need 3 args */
	      field_number_set = TRUE;
	      }
            }
          }
        else
	  {
	  expand_string_message = string_sprintf(
	    "missing '{' for arg %d of extract", i+1);
	  goto EXPAND_FAILED_CURLY;
	  }
        }

      /* Extract either the numbered or the keyed substring into $value. If
      skipping, just pretend the extraction failed. */

      if (flags & ESI_SKIPPING)
	lookup_value = NULL;
      else switch (fmt)
	{
	case extract_basic:
	  lookup_value = field_number_set
	    ? expand_gettokened(field_number, sub[1], sub[2])
	    : expand_getkeyed(sub[0], sub[1]);
	  break;

	case extract_json:
	case extract_jsons:
	  {
	  uschar * t, * item;
	  const uschar * list;

	  /* Array: Bracket-enclosed and comma-separated.
	  Object: Brace-enclosed, comma-sep list of name:value pairs */

	  if (!(t = dewrap(sub[1], field_number_set ? US"[]" : US"{}")))
	    {
	    expand_string_message =
	      string_sprintf("%s wrapping %s for extract json",
		expand_string_message,
		field_number_set ? "array" : "object");
	    goto EXPAND_FAILED_CURLY;
	    }

	  list = t;
	  if (field_number_set)
	    {
	    if (field_number <= 0)
	      {
	      expand_string_message = US"first argument of \"extract\" must "
		"be greater than zero";
	      goto EXPAND_FAILED;
	      }
	    while (field_number > 0 && (item = json_nextinlist(&list)))
	      field_number--;
	    if ((lookup_value = t = item))
	      {
	      while (*t) t++;
	      while (--t >= lookup_value && isspace(*t)) *t = '\0';
	      }
	    }
	  else
	    {
	    lookup_value = NULL;
	    while ((item = json_nextinlist(&list)))
	      {
	      /* Item is:  string name-sep value.  string is quoted.
	      Dequote the string and compare with the search key. */

	      if (!(item = dewrap(item, US"\"\"")))
		{
		expand_string_message =
		  string_sprintf("%s wrapping string key for extract json",
		    expand_string_message);
		goto EXPAND_FAILED_CURLY;
		}
	      if (Ustrcmp(item, sub[0]) == 0)	/*XXX should be a UTF8-compare */
		{
		t = item + Ustrlen(item) + 1;
		if (Uskip_whitespace(&t) != ':')
		  {
		  expand_string_message =
		    US"missing object value-separator for extract json";
		  goto EXPAND_FAILED_CURLY;
		  }
		t++;
		Uskip_whitespace(&t);
		lookup_value = t;
		break;
		}
	      }
	    }
	  }

	  if (  fmt == extract_jsons
	     && lookup_value
	     && !(lookup_value = dewrap(lookup_value, US"\"\"")))
	    {
	    expand_string_message =
	      string_sprintf("%s wrapping string result for extract jsons",
		expand_string_message);
	    goto EXPAND_FAILED_CURLY;
	    }
	  break;	/* json/s */
	}

      /* If no string follows, $value gets substituted; otherwise there can
      be yes/no strings, as for lookup or if. */

      switch(process_yesno(
               flags,			/* were previously skipping */
               lookup_value != NULL,	/* success/failure indicator */
               save_lookup_value,	/* value to reset for string2 */
               &s,			/* input pointer */
               &yield,			/* output pointer */
               US"extract",		/* condition type */
	       &resetok))
        {
        case 1: goto EXPAND_FAILED;          /* when all is well, the */
        case 2: goto EXPAND_FAILED_CURLY;    /* returned value is 0 */
        }

      /* All done - restore numerical variables. */

      restore_expand_strings(save_expand_nmax, save_expand_nstring,
        save_expand_nlength);

      if (flags & ESI_SKIPPING) continue; else break;
      }

    /* return the Nth item from a list */

    case EITEM_LISTEXTRACT:
      {
      int field_number = 1, sep = 0;
      uschar * save_lookup_value = lookup_value, * sub[2];
      int save_expand_nmax =
        save_expand_strings(save_expand_nstring, save_expand_nlength);

      /* Read the field & list arguments */
      /*XXX Could we use read_subs here (and get better efficiency for skipping)? */

      for (int i = 0; i < 2; i++)
        {
        if (Uskip_whitespace(&s) != '{')				/*}*/
	  {
	  expand_string_message = string_sprintf(
	    "missing '{' for arg %d of listextract", i+1);		/*}*/
	  goto EXPAND_FAILED_CURLY;
	  }

	s++;
	if (i == 1) sep = matchlist_parse_sep(&s);

	sub[i] = expand_string_internal(s,
	      ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL);
	if (!sub[i])     goto EXPAND_FAILED;				/*{{*/
	if (*s++ != '}')
	  {
	  expand_string_message = string_sprintf(
	    "missing '}' closing arg %d of listextract", i+1);
	  goto EXPAND_FAILED_CURLY;
	  }

	/* After removal of leading and trailing white space, the first
	argument must be numeric and nonempty. */

	if (i == 0)
	  {
	  int len;
	  int x = 0;
	  uschar *p = sub[0];

	  Uskip_whitespace(&p);
	  sub[0] = p;

	  len = Ustrlen(p);
	  while (len > 0 && isspace(p[len-1])) len--;
	  p[len] = 0;

	  if (!*p && !(flags & ESI_SKIPPING))
	    {
	    expand_string_message = US"first argument of \"listextract\" must "
	      "not be empty";
	    goto EXPAND_FAILED;
	    }

	  if (*p == '-')
	    {
	    field_number = -1;
	    p++;
	    }
	  while (*p && isdigit(*p)) x = x * 10 + *p++ - '0';
	  if (*p)
	    {
	    expand_string_message = US"first argument of \"listextract\" must "
	      "be numeric";
	    goto EXPAND_FAILED;
	    }
	  field_number *= x;
	  }
        }

      /* Extract the numbered element into $value. If
      skipping, just pretend the extraction failed. */

      lookup_value = flags & ESI_SKIPPING
	? NULL : expand_getlistele(field_number, sub[1], sep);

      /* If no string follows, $value gets substituted; otherwise there can
      be yes/no strings, as for lookup or if. */

      switch(process_yesno(
               flags,				/* were previously skipping */
               lookup_value != NULL,		/* success/failure indicator */
               save_lookup_value,		/* value to reset for string2 */
               &s,				/* input pointer */
               &yield,				/* output pointer */
               US"listextract",			/* condition type */
	       &resetok))
        {
        case 1: goto EXPAND_FAILED;          /* when all is well, the */
        case 2: goto EXPAND_FAILED_CURLY;    /* returned value is 0 */
        }

      /* All done - restore numerical variables. */

      restore_expand_strings(save_expand_nmax, save_expand_nstring,
        save_expand_nlength);

      if (flags & ESI_SKIPPING) continue; else break;
      }

    case EITEM_LISTQUOTE:
      {
      uschar * sub[2];
      switch(read_subs(sub, 2, 2, &s, flags, TRUE, name, &resetok, NULL))
        {
	case -1: continue;	/* skipping */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }
      if (*sub[1]) for (uschar sep = *sub[0], c; c = *sub[1]; sub[1]++)
	{
	if (c == sep) yield = string_catn(yield, sub[1], 1);
	yield = string_catn(yield, sub[1], 1);
	}
      else yield = string_catn(yield, US" ", 1);
      break;
      }

#ifndef DISABLE_TLS
    case EITEM_CERTEXTRACT:
      {
      uschar * save_lookup_value = lookup_value, * sub[2];
      int save_expand_nmax =
        save_expand_strings(save_expand_nstring, save_expand_nlength);

      /* Read the field argument */
      if (Uskip_whitespace(&s) != '{')					/*}*/
	{
	expand_string_message = US"missing '{' for field arg of certextract";
	goto EXPAND_FAILED_CURLY;					/*}*/
	}
      sub[0] = expand_string_internal(s+1,
		ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL);
      if (!sub[0])     goto EXPAND_FAILED;				/*{{*/
      if (*s++ != '}')
        {
	expand_string_message = US"missing '}' closing field arg of certextract";
	goto EXPAND_FAILED_CURLY;
	}
      /* strip spaces fore & aft */
      {
      int len;
      uschar *p = sub[0];

      Uskip_whitespace(&p);
      sub[0] = p;

      len = Ustrlen(p);
      while (len > 0 && isspace(p[len-1])) len--;
      p[len] = 0;
      }

      /* inspect the cert argument */
      if (Uskip_whitespace(&s) != '{')					/*}*/
	{
	expand_string_message = US"missing '{' for cert variable arg of certextract";
	goto EXPAND_FAILED_CURLY;					/*}*/
	}
      if (*++s != '$')
        {
	expand_string_message = US"second argument of \"certextract\" must "
	  "be a certificate variable";
	goto EXPAND_FAILED;
	}
      sub[1] = expand_string_internal(s+1,
		ESI_BRACE_ENDS | flags & ESI_SKIPPING, &s, &resetok, NULL);
      if (!sub[1])     goto EXPAND_FAILED;				/*{{*/
      if (*s++ != '}')
        {
	expand_string_message = US"missing '}' closing cert variable arg of certextract";
	goto EXPAND_FAILED_CURLY;
	}

      if (flags & ESI_SKIPPING)
	lookup_value = NULL;
      else
	{
	lookup_value = expand_getcertele(sub[0], sub[1]);
	if (*expand_string_message) goto EXPAND_FAILED;
	}
      switch(process_yesno(
               flags,				/* were previously skipping */
               lookup_value != NULL,		/* success/failure indicator */
               save_lookup_value,		/* value to reset for string2 */
               &s,				/* input pointer */
               &yield,				/* output pointer */
               US"certextract",			/* condition type */
	       &resetok))
        {
        case 1: goto EXPAND_FAILED;          /* when all is well, the */
        case 2: goto EXPAND_FAILED_CURLY;    /* returned value is 0 */
        }

      restore_expand_strings(save_expand_nmax, save_expand_nstring,
        save_expand_nlength);
      if (flags & ESI_SKIPPING) continue; else break;
      }
#endif	/*DISABLE_TLS*/

    /* Handle list operations */

    case EITEM_FILTER:
    case EITEM_MAP:
    case EITEM_REDUCE:
      {
      int sep, save_ptr = gstring_length(yield);
      uschar outsep[2] = { '\0', '\0' };
      const uschar *list, *expr, *temp;
      uschar * save_iterate_item = iterate_item;
      uschar * save_lookup_value = lookup_value;

      Uskip_whitespace(&s);
      if (*s++ != '{')							/*}*/
        {
	expand_string_message =
	  string_sprintf("missing '{' for first arg of %s", name);
	goto EXPAND_FAILED_CURLY;					/*}*/
	}

      DEBUG(D_expand) debug_printf_indent("%s: evaluate input list list\n", name);
      /* Check for a list-sep spec before expansion */
      sep = matchlist_parse_sep(&s);

      if (!(list = expand_string_internal(s,
	      ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL)))
	goto EXPAND_FAILED;						/*{{*/
      if (*s++ != '}')
        {
	expand_string_message =
	  string_sprintf("missing '}' closing first arg of %s", name);
	goto EXPAND_FAILED_CURLY;
	}

      if (item_type == EITEM_REDUCE)
        {
	uschar * t;
        Uskip_whitespace(&s);
        if (*s++ != '{')						/*}*/
	  {
	  expand_string_message = US"missing '{' for second arg of reduce";
	  goto EXPAND_FAILED_CURLY;					/*}*/
	  }
	DEBUG(D_expand) debug_printf_indent("reduce: initial result list\n");
        t = expand_string_internal(s,
	      ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL);
        if (!t) goto EXPAND_FAILED;
        lookup_value = t;						/*{{*/
        if (*s++ != '}')
	  {
	  expand_string_message = US"missing '}' closing second arg of reduce";
	  goto EXPAND_FAILED_CURLY;
	  }
        }

      Uskip_whitespace(&s);
      if (*s++ != '{')							/*}*/
        {
	expand_string_message =
	  string_sprintf("missing '{' for last arg of %s", name);	/*}*/
	goto EXPAND_FAILED_CURLY;
	}

      expr = s;

      /* For EITEM_FILTER, call eval_condition once, with result discarded (as
      if scanning a "false" part). This allows us to find the end of the
      condition, because if the list is empty, we won't actually evaluate the
      condition for real. For EITEM_MAP and EITEM_REDUCE, do the same, using
      the normal internal expansion function. */

      DEBUG(D_expand) debug_printf_indent("%s: find end of conditionn\n", name);
      if (item_type != EITEM_FILTER)
        temp = expand_string_internal(s,
	  ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | ESI_SKIPPING, &s, &resetok, NULL);
      else
        if ((temp = eval_condition(expr, &resetok, NULL))) s = temp;

      if (!temp)
        {
        expand_string_message = string_sprintf("%s inside %q item",
          expand_string_message, name);
        goto EXPAND_FAILED;
        }

      Uskip_whitespace(&s);						/*{{{*/
      if (*s++ != '}')
        {
        expand_string_message = string_sprintf("missing } at end of condition "
          "or expression inside %q; could be an unquoted } in the content",
	  name);
        goto EXPAND_FAILED;
        }

      Uskip_whitespace(&s);						/*{{*/
      if (*s++ != '}')
        {
        expand_string_message = string_sprintf("missing } at end of %q",
          name);
        goto EXPAND_FAILED;
        }

      /* If we are skipping, we can now just move on to the next item. When
      processing for real, we perform the iteration. */

      if (flags & ESI_SKIPPING) continue;
      while ((iterate_item = string_nextinlist(&list, &sep, NULL, 0)))
        {
        *outsep = (uschar)sep;      /* Separator as a string */

	DEBUG(D_expand) debug_printf_indent("%s: $item = '%s'  $value = '%s'\n",
			  name, iterate_item, lookup_value);

        if (item_type == EITEM_FILTER)
          {
          BOOL condresult;
	  /* the condition could modify $value, as a side-effect */
	  uschar * save_value = lookup_value;

          if (!eval_condition(expr, &resetok, &condresult))
            {
            iterate_item = save_iterate_item;
            lookup_value = save_lookup_value;
            expand_string_message = string_sprintf(
	      "%q inside %q condition", expand_string_message, name);
            goto EXPAND_FAILED;
            }
	  lookup_value = save_value;
          DEBUG(D_expand) debug_printf_indent("%s: condition is %s\n", name,
            condresult? "true":"false");
          if (condresult)
            temp = iterate_item;    /* TRUE => include this item */
          else
            continue;               /* FALSE => skip this item */
          }

        else			/* EITEM_MAP and EITEM_REDUCE */
          {
	  /* the expansion could modify $value, as a side-effect */
	  uschar * t = expand_string_internal(expr,
	    ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, NULL, &resetok, NULL);
          if (!(temp = t))
            {
            iterate_item = save_iterate_item;
            expand_string_message = string_sprintf("%q inside %q item",
              expand_string_message, name);
            goto EXPAND_FAILED;
            }
          if (item_type == EITEM_REDUCE)
            {
            lookup_value = t;         /* Update the value of $value */
            continue;                 /* and continue the iteration */
            }
          }

        /* We reach here for FILTER if the condition is true, always for MAP,
        and never for REDUCE. The value in "temp" is to be added to the output
        list that is being created, ensuring that any occurrences of the
        separator character are doubled. Unless we are dealing with the first
        item of the output list, add in a space if the new item begins with the
        separator character, or is an empty string. */

/*XXX is there not a standard support function for this, appending to a list? */
/* yes, string_append_listele(), but it depends on lack of text before the list */

        if (  yield && yield->ptr != save_ptr
	   && (temp[0] == *outsep || temp[0] == 0))
          yield = string_catn(yield, US" ", 1);

        /* Add the string in "temp" to the output list that we are building,
        This is done in chunks by searching for the separator character. */

        for (;;)
          {
          size_t seglen = Ustrcspn(temp, outsep);

	  yield = string_catn(yield, temp, seglen + 1);

          /* If we got to the end of the string we output one character
          too many; backup and end the loop. Otherwise arrange to double the
          separator. */

          if (!temp[seglen]) { yield->ptr--; break; }
          yield = string_catn(yield, outsep, 1);
          temp += seglen + 1;
          }

        /* Output a separator after the string: we will remove the redundant
        final one at the end. */

        yield = string_catn(yield, outsep, 1);
        }   /* End of iteration over the list loop */

      /* REDUCE has generated no output above: output the final value of
      $value. */

      if (item_type == EITEM_REDUCE)
        {
        yield = string_cat(yield, lookup_value);
        lookup_value = save_lookup_value;  /* Restore $value */
        }

      /* FILTER and MAP generate lists: if they have generated anything, remove
      the redundant final separator. Even though an empty item at the end of a
      list does not count, this is tidier. */

      else if (yield && yield->ptr != save_ptr) yield->ptr--;

      /* Restore preserved $item */

      iterate_item = save_iterate_item;
      if (flags & ESI_SKIPPING) continue; else break;
      }

    case EITEM_SORT:
      {
      int sep, cond_type;
      const uschar * srclist, * cmp, * xtract;
      uschar * opname, * srcitem;
      const uschar * dstlist = NULL, * dstkeylist = NULL;
      uschar * save_iterate_item = iterate_item;

      Uskip_whitespace(&s);
      if (*s++ != '{')							/*}*/
        {
        expand_string_message = US"missing '{' for list arg of sort";
	goto EXPAND_FAILED_CURLY;					/*}*/
	}

      sep = matchlist_parse_sep(&s);
      srclist = expand_string_internal(s,
	      ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL);
      if (!srclist) goto EXPAND_FAILED;					/*{{*/
      if (*s++ != '}')
        {
        expand_string_message = US"missing '}' closing list arg of sort";
	goto EXPAND_FAILED_CURLY;
	}

      Uskip_whitespace(&s);
      if (*s++ != '{')							/*}*/
        {
        expand_string_message = US"missing '{' for comparator arg of sort";
	goto EXPAND_FAILED_CURLY;					/*}*/
	}

      cmp = expand_string_internal(s,
	      ESI_BRACE_ENDS | flags & ESI_SKIPPING, &s, &resetok, NULL);
      if (!cmp) goto EXPAND_FAILED;					/*{{*/
      if (*s++ != '}')
        {
        expand_string_message = US"missing '}' closing comparator arg of sort";
	goto EXPAND_FAILED_CURLY;
	}

      if ((cond_type = identify_operator(&cmp, &opname)) == -1)
	{
	if (!expand_string_message)
	  expand_string_message = string_sprintf("unknown condition %q", s);
	goto EXPAND_FAILED;
	}
      switch(cond_type)
	{
	case ECOND_NUM_L: case ECOND_NUM_LE:
	case ECOND_NUM_G: case ECOND_NUM_GE:
	case ECOND_STR_GE: case ECOND_STR_GEI: case ECOND_STR_GT: case ECOND_STR_GTI:
	case ECOND_STR_LE: case ECOND_STR_LEI: case ECOND_STR_LT: case ECOND_STR_LTI:
	  break;

	default:
	  expand_string_message = US"comparator not handled for sort";
	  goto EXPAND_FAILED;
	}

      Uskip_whitespace(&s);
      if (*s++ != '{')							/*}*/
        {
        expand_string_message = US"missing '{' for extractor arg of sort";
	goto EXPAND_FAILED_CURLY;					/*}*/
	}

      xtract = s;
      if (!expand_string_internal(s,
	ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | ESI_SKIPPING, &s, &resetok, NULL))
	goto EXPAND_FAILED;
      xtract = string_copyn(xtract, s - xtract);
									/*{{*/
      if (*s++ != '}')
        {
        expand_string_message = US"missing '}' closing extractor arg of sort";
	goto EXPAND_FAILED_CURLY;
	}
									/*{{*/
      if (*s++ != '}')
        {
        expand_string_message = US"missing } at end of \"sort\"";
        goto EXPAND_FAILED;
        }

      if (flags & ESI_SKIPPING) continue;

      while ((srcitem = string_nextinlist(&srclist, &sep, NULL, 0)))
	{
	const uschar * srcfield, * dstitem;
	gstring * newlist = NULL, * newkeylist = NULL;

        DEBUG(D_expand) debug_printf_indent("%s: $item = %q\n", name, srcitem);

	/* extract field for comparisons */
	iterate_item = srcitem;
	if (  !(srcfield = expand_string_internal(xtract,
				  ESI_HONOR_DOLLAR, NULL, &resetok, NULL))
	   || !*srcfield)
	  {
	  expand_string_message = string_sprintf("field-extract in sort: %q",
						  xtract);
	  goto EXPAND_FAILED;
	  }

	/* Insertion sort */

	/* copy output list until new-item < list-item */
	while ((dstitem = string_nextinlist(&dstlist, &sep, NULL, 0)))
	  {
	  const uschar * dstfield;

	  /* field for comparison */
	  if (!(dstfield = string_nextinlist(&dstkeylist, &sep, NULL, 0)))
	    goto SORT_MISMATCH;

	  /* String-comparator names start with a letter; numeric names do not */

	  if (sortsbefore(cond_type, isalpha(opname[0]),
	      srcfield, dstfield))
	    {
	    /* New-item sorts before this dst-item.  Append new-item,
	    then dst-item, then remainder of dst list. */

	    newlist = string_append_listele(newlist, sep, srcitem);
	    newkeylist = string_append_listele(newkeylist, sep, srcfield);
	    srcitem = NULL;

	    newlist = string_append_listele(newlist, sep, dstitem);
	    newkeylist = string_append_listele(newkeylist, sep, dstfield);

/*XXX why field-at-a-time copy?  Why not just dup the rest of the list? */
	    while ((dstitem = string_nextinlist(&dstlist, &sep, NULL, 0)))
	      {
	      if (!(dstfield = string_nextinlist(&dstkeylist, &sep, NULL, 0)))
		goto SORT_MISMATCH;
	      newlist = string_append_listele(newlist, sep, dstitem);
	      newkeylist = string_append_listele(newkeylist, sep, dstfield);
	      }

	    break;
	    }

	  newlist = string_append_listele(newlist, sep, dstitem);
	  newkeylist = string_append_listele(newkeylist, sep, dstfield);
	  }

	/* If we ran out of dstlist without consuming srcitem, append it */
	if (srcitem)
	  {
	  newlist = string_append_listele(newlist, sep, srcitem);
	  newkeylist = string_append_listele(newkeylist, sep, srcfield);
	  }

	dstlist = newlist->s;
	dstkeylist = newkeylist->s;

        DEBUG(D_expand) debug_printf_indent("%s: dstlist = %q\n", name, dstlist);
        DEBUG(D_expand) debug_printf_indent("%s: dstkeylist = %q\n", name, dstkeylist);
	}

      if (dstlist)
	yield = string_cat(yield, dstlist);

      /* Restore preserved $item */
      iterate_item = save_iterate_item;
      break;

      SORT_MISMATCH:
	expand_string_message = US"Internal error in sort (list mismatch)";
	goto EXPAND_FAILED;
      }


    /* If ${dlfunc } support is configured, handle calling dynamically-loaded
    functions, unless locked out at this time. Syntax is ${dlfunc{file}{func}}
    or ${dlfunc{file}{func}{arg}} or ${dlfunc{file}{func}{arg1}{arg2}} or up to
    a maximum of EXPAND_DLFUNC_MAX_ARGS arguments (defined below). */

    #define EXPAND_DLFUNC_MAX_ARGS 8

    case EITEM_DLFUNC:
#ifndef EXPAND_DLFUNC
      expand_string_message = US"\"${dlfunc\" encountered, but this facility "	/*}*/
	"is not included in this binary";
      goto EXPAND_FAILED;

#else   /* EXPAND_DLFUNC */
      {
      tree_node * t;
      exim_dlfunc_t * func;
      uschar * result;
      int status, argc;
      uschar * argv[EXPAND_DLFUNC_MAX_ARGS + 3];

      if (expand_forbid & RDO_DLFUNC)
        {
        expand_string_message =
          US"dynamically-loaded functions are not permitted";
        goto EXPAND_FAILED;
        }

      switch(read_subs(argv, EXPAND_DLFUNC_MAX_ARGS + 2, 2, &s, flags,
           TRUE, name, &resetok, NULL))
        {
	case -1: continue;	/* skipping */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }

      /* Look up the dynamically loaded object handle in the tree. If it isn't
      found, dlopen() the file and put the handle in the tree for next time. */

      if (!(t = tree_search(dlobj_anchor, argv[0])))
        {
        void * handle = dlopen(CS argv[0], RTLD_LAZY);
        if (!handle)
          {
          expand_string_message = string_sprintf("dlopen %q failed: %s",
            argv[0], dlerror());
          log_write(0, LOG_MAIN|LOG_PANIC, "%s", expand_string_message);
          goto EXPAND_FAILED;
          }
        t = store_get_perm(sizeof(tree_node) + Ustrlen(argv[0]), argv[0]);
        Ustrcpy(t->name, argv[0]);
        t->data.ptr = handle;
        (void)tree_insertnode(&dlobj_anchor, t);
        }

      /* Having obtained the dynamically loaded object handle, look up the
      function pointer. */

      if (!(func = (exim_dlfunc_t *)dlsym(t->data.ptr, CS argv[1])))
        {
        expand_string_message = string_sprintf("dlsym %q in %q failed: "
          "%s", argv[1], argv[0], dlerror());
        log_write(0, LOG_MAIN|LOG_PANIC, "%s", expand_string_message);
        goto EXPAND_FAILED;
        }

      /* Call the function and work out what to do with the result. If it
      returns OK, we have a replacement string; if it returns DEFER then
      expansion has failed in a non-forced manner; if it returns FAIL then
      failure was forced; if it returns ERROR or any other value there's a
      problem, so panic slightly. In any case, assume that the function has
      side-effects on the store that must be preserved. */

      resetok = FALSE;
      result = NULL;
      for (argc = 0; argv[argc]; argc++) ;

      if ((status = func(&result, argc - 2, &argv[2])) != OK)
        {
        expand_string_message = result ? result : US"(no message)";
        if (status == FAIL_FORCED)
	  f.expand_string_forcedfail = TRUE;
	else if (status != FAIL)
	  log_write(0, LOG_MAIN|LOG_PANIC, "dlfunc{%s}{%s} failed (%d): %s",
              argv[0], argv[1], status, expand_string_message);
        goto EXPAND_FAILED;
        }

      if (result) yield = string_cat(yield, result);
      break;
      }
#endif /* EXPAND_DLFUNC */

    case EITEM_ENV:	/* ${env {name} {val_if_found} {val_if_unfound}} */
      {
      const uschar * key;
      uschar * save_lookup_value = lookup_value;

      if (Uskip_whitespace(&s) != '{')					/*}*/
	goto EXPAND_FAILED;

      key = expand_string_internal(s+1,
	      ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL);
      if (!key) goto EXPAND_FAILED;					/*{{*/
      if (*s++ != '}')
        {
        expand_string_message = US"missing '}' for name arg of env";
	goto EXPAND_FAILED_CURLY;
	}

      lookup_value = US getenv(CS key);

      switch(process_yesno(
               flags,				/* were previously skipping */
               lookup_value != NULL,		/* success/failure indicator */
               save_lookup_value,		/* value to reset for string2 */
               &s,				/* input pointer */
               &yield,				/* output pointer */
               US"env",				/* condition type */
	       &resetok))
        {
        case 1: goto EXPAND_FAILED;          /* when all is well, the */
        case 2: goto EXPAND_FAILED_CURLY;    /* returned value is 0 */
        }
      if (flags & ESI_SKIPPING) continue; else break;
      }

#ifdef SUPPORT_SRS
    case EITEM_SRS_ENCODE:
      /* ${srs_encode {secret} {return_path} {orig_domain}} */
      {
      uschar * sub[3];
      uschar cksum[4];
      gstring * g = NULL;
      BOOL quoted = FALSE;

      switch (read_subs(sub, 3, 3, CUSS &s, flags, TRUE, name, &resetok, NULL))
        {
	case -1: continue;	/* skipping */
        case 1: goto EXPAND_FAILED_CURLY;
        case 2:
        case 3: goto EXPAND_FAILED;
        }
      if (flags & ESI_SKIPPING) continue;

      if (sub[1] && *(sub[1]))
	{
	g = string_catn(g, US"SRS0=", 5);

	/* ${l_4:${hmac{md5}{SRS_SECRET}{${lc:$return_path}}}}= */
	hmac_md5(sub[0], string_copylc(sub[1]), cksum, sizeof(cksum));
	g = string_catn(g, cksum, sizeof(cksum));
	g = string_catn(g, US"=", 1);

	/* ${base32:${eval:$tod_epoch/86400&0x3ff}}= */
	  {
	  struct timeval now;
	  unsigned long i;

	  gettimeofday(&now, NULL);
	  i = (now.tv_sec / 86400) & 0x3ff;
	  g = string_catn(g, &base32_chars[i >> 5], 1);
	  g = string_catn(g, &base32_chars[i & 0x1f], 1);
	  }
	g = string_catn(g, US"=", 1);

	/* ${domain:$return_path}=${local_part:$return_path} */
	  {
	  int start, end, domain;
	  uschar * t = parse_extract_address(sub[1], &expand_string_message,
					    &start, &end, &domain, FALSE);
	  uschar * ss;

	  if (!t)
	    goto EXPAND_FAILED;

	  if (domain > 0) g = string_cat(g, t + domain);
	  g = string_catn(g, US"=", 1);

	  ss = domain > 0 ? string_copyn(t, domain - 1) : t;
	  if ((quoted = Ustrchr(ss, '"') != NULL))
	    {
	    gstring * h = NULL;
	    DEBUG(D_expand) debug_printf_indent("auto-quoting local part\n");
	    while (*ss)		/* de-quote */
	      {
	      while (*ss && *ss != '"') h = string_catn(h, ss++, 1);
	      if (*ss) ss++;
	      while (*ss && *ss != '"') h = string_catn(h, ss++, 1);
	      if (*ss) ss++;
	      }
	    gstring_release_unused(h);
	    ss = string_from_gstring(h);
	    }
	  if (ss) g = string_cat(g, ss);
	  }

	/* Assume that if the original local_part had quotes
	it was for good reason */

	if (quoted) yield = string_catn(yield, US"\"", 1);
	yield = gstring_append(yield, g);
	if (quoted) yield = string_catn(yield, US"\"", 1);

	/* @$original_domain */
	yield = string_catn(yield, US"@", 1);
	yield = string_cat(yield, sub[2]);
	}
      else
	DEBUG(D_expand) debug_printf_indent("null return_path for srs-encode\n");

      break;
      }
#endif /*SUPPORT_SRS*/

    default:
      goto NOT_ITEM;
    }	/* EITEM_* switch */
    /*NOTREACHED*/

  DEBUG(D_expand)		/* only if not the sole expansion of the line */
    if (yield && (expansion_start > 0 || *s))
      debug_expansion_interim(US"item-res",
	  yield->s + expansion_start, yield->ptr - expansion_start,
	  flags);
  continue;

NOT_ITEM: ;
  }

  /* Control reaches here if the name is not recognized as one of the more
  complicated expansion items. Check for the "operator" syntax (name terminated
  by a colon). Some of the operators have arguments, separated by _ from the
  name. */

  if (*s == ':')
    {
    int c;
    uschar * arg = NULL, * sub;
#ifndef DISABLE_TLS
    var_entry * vp = NULL;
#endif

    /* Owing to an historical mis-design, an underscore may be part of the
    operator name, or it may introduce arguments.  We therefore first scan the
    table of names that contain underscores. If there is no match, we cut off
    the arguments and then scan the main table. */

    if ((c = chop_match(name, op_table_underscore,
			nelem(op_table_underscore))) < 0)
      {
      if ((arg = Ustrchr(name, '_')))
	*arg = 0;
      if ((c = chop_match(name, op_table_main, nelem(op_table_main))) >= 0)
	c += nelem(op_table_underscore);
      if (arg) *arg++ = '_';		/* Put back for error messages */
      }

    /* Deal specially with operators that might take a certificate variable
    as we do not want to do the usual expansion. For most, expand the string.*/

    switch(c)
      {
#ifndef DISABLE_TLS
      case EOP_MD5:
      case EOP_SHA1:
      case EOP_SHA256:
      case EOP_BASE64:
	if (s[1] == '$')
	  {
	  const uschar * s1 = s;
	  sub = expand_string_internal(s+2,
	      ESI_BRACE_ENDS | flags & ESI_SKIPPING, &s1, &resetok, NULL);
	  if (!sub)       goto EXPAND_FAILED;		/*{*/
	  if (*s1 != '}')
	    {						/*{*/
	    expand_string_message =
	      string_sprintf("missing '}' closing cert arg of %s", name);
	    goto EXPAND_FAILED_CURLY;
	    }
	  if (  (vp = find_var_ent(sub, var_table, nelem(var_table)))
	     && vp->type == vtype_cert)
	    {
	    s = s1+1;
	    break;
	    }
	  vp = NULL;
	  }
        /*FALLTHROUGH*/
#endif
      default:
	sub = expand_string_internal(s+1,
		ESI_BRACE_ENDS | ESI_HONOR_DOLLAR | flags, &s, &resetok, NULL);
	if (!sub) goto EXPAND_FAILED;
	s++;
	break;
      }

    /* If we are skipping, we don't need to perform the operation at all.
    This matters for operations like "mask", because the data may not be
    in the correct format when skipping. For example, the expression may test
    for the existence of $sender_host_address before trying to mask it. For
    other operations, doing them may not fail, but it is a waste of time. */

    if (flags & ESI_SKIPPING && c >= 0) continue;

    /* Otherwise, switch on the operator type.  After handling go back
    to the main loop top. */

     {
     unsigned expansion_start = gstring_length(yield);
     switch(c)
      {
      case EOP_BASE32:
	{
	uschar * t;
	unsigned long int n = Ustrtoul(sub, &t, 10);
	gstring * g = NULL;

	if (*t)
	  {
	  expand_string_message = string_sprintf("argument for base32 "
	    "operator is %q, which is not a decimal number", sub);
	  goto EXPAND_FAILED;
	  }
	for ( ; n; n >>= 5)
	  g = string_catn(g, &base32_chars[n & 0x1f], 1);

	if (g) while (g->ptr > 0) yield = string_catn(yield, &g->s[--g->ptr], 1);
	break;
	}

      case EOP_BASE32D:
	{
	const uschar * tt = sub;
	unsigned long int n = 0;
	while (*tt)
	  {
	  const uschar * t = Ustrchr(base32_chars, *tt++);
	  if (!t)
	    {
	    expand_string_message = string_sprintf("argument for base32d "
	      "operator is %q, which is not a base 32 number", sub);
	    goto EXPAND_FAILED;
	    }
	  n = n * 32 + (t - base32_chars);
	  }
	yield = string_fmt_append(yield, "%ld", n);
	break;
	}

      case EOP_BASE62:
	{
	uschar *t;
	unsigned long int n = Ustrtoul(sub, &t, 10);
	if (*t)
	  {
	  expand_string_message = string_sprintf("argument for base62 "
	    "operator is %q, which is not a decimal number", sub);
	  goto EXPAND_FAILED;
	  }
	yield = string_cat(yield, string_base62_32(n));		/*XXX only handles 32b input range.  Need variants? */
	break;
	}

      /* Note that for Darwin and Cygwin, BASE_62 actually has the value 36 */

      case EOP_BASE62D:
	{
	const uschar * tt = sub;
	unsigned long int n = 0;
	while (*tt)
	  {
	  const uschar * t = Ustrchr(base62_chars, *tt++);
	  if (!t)
	    {
	    expand_string_message = string_sprintf("argument for base62d "
	      "operator is %q, which is not a base %d number", sub,
	      BASE_62);
	    goto EXPAND_FAILED;
	    }
	  n = n * BASE_62 + (t - base62_chars);
	  }
	yield = string_fmt_append(yield, "%ld", n);
	break;
	}

      case EOP_EXPAND:
	{
	const uschar * expanded = expand_string_internal(sub,
		ESI_HONOR_DOLLAR | flags & ESI_SKIPPING, NULL, &resetok, NULL);
	if (!expanded)
	  {
	  expand_string_message =
	    string_sprintf("internal expansion of %q failed: %s", sub,
	      expand_string_message);
	  goto EXPAND_FAILED;
	  }
	yield = string_cat(yield, expanded);
	break;
	}

      case EOP_LC:
	{
	uschar * t = sub - 1;
	while (*++t) *t = tolower(*t);
	yield = string_catn(yield, sub, t-sub);
	break;
	}

      case EOP_UC:
	{
	uschar * t = sub - 1;
	while (*++t) *t = toupper(*t);
	yield = string_catn(yield, sub, t-sub);
	break;
	}

      case EOP_MD5:
#ifndef DISABLE_TLS
	if (vp && *(void **)vp->value)
	  {
	  const uschar * cp = tls_cert_fprt_md5(*(void **)vp->value);
	  yield = string_cat(yield, cp);
	  }
	else
#endif
	  {
	  md5 base;
	  uschar digest[16];
	  md5_start(&base);
	  md5_end(&base, sub, Ustrlen(sub), digest);
	  yield = string_fmt_append(yield, "%.16H", digest);
	  }
	break;

      case EOP_SHA1:
#ifndef DISABLE_TLS
	if (vp && *(void **)vp->value)
	  {
	  uschar * cp = tls_cert_fprt_sha1(*(void **)vp->value);
	  yield = string_cat(yield, cp);
	  }
	else
#endif
	  {
	  hctx h;
	  uschar digest[20];
	  sha1_start(&h);
	  sha1_end(&h, sub, Ustrlen(sub), digest);
	  yield = string_fmt_append(yield, "%#.20H", digest);
	  }
	break;

      case EOP_SHA2:
      case EOP_SHA256:
#ifdef EXIM_HAVE_SHA2
	if (vp && *(void **)vp->value)
	  if (c == EOP_SHA256)
	    yield = string_cat(yield, tls_cert_fprt_sha256(*(void **)vp->value));
	  else
	    expand_string_message = US"sha2_N not supported with certificates";
	else
	  {
	  hctx h;
	  blob b;
	  hashmethod m = !arg ? HASH_SHA2_256
	    : Ustrcmp(arg, "256") == 0 ? HASH_SHA2_256
	    : Ustrcmp(arg, "384") == 0 ? HASH_SHA2_384
	    : Ustrcmp(arg, "512") == 0 ? HASH_SHA2_512
	    : HASH_BADTYPE;

	  if (m == HASH_BADTYPE || !exim_sha_init(&h, m))
	    {
	    expand_string_message = US"unrecognised sha2 variant";
	    goto EXPAND_FAILED;
	    }

	  exim_sha_update_string(&h, sub);
	  exim_sha_finish(&h, &b);
	  yield = string_fmt_append(yield, "%#.*H", (int)b.len, b.data);
	  }
#else
	  expand_string_message = US"sha256 only supported with TLS";
#endif
	break;

      case EOP_SHA3:
#ifdef EXIM_HAVE_SHA3
	{
	hctx h;
	blob b;
	hashmethod m = !arg ? HASH_SHA3_256
	  : Ustrcmp(arg, "224") == 0 ? HASH_SHA3_224
	  : Ustrcmp(arg, "256") == 0 ? HASH_SHA3_256
	  : Ustrcmp(arg, "384") == 0 ? HASH_SHA3_384
	  : Ustrcmp(arg, "512") == 0 ? HASH_SHA3_512
	  : HASH_BADTYPE;

	if (m == HASH_BADTYPE || !exim_sha_init(&h, m))
	  {
	  expand_string_message = US"unrecognised sha3 variant";
	  goto EXPAND_FAILED;
	  }

	exim_sha_update_string(&h, sub);
	exim_sha_finish(&h, &b);
	yield = string_fmt_append(yield, "%#.*H", (int)b.len, b.data);
	}
	break;
#else
	expand_string_message = US"sha3 only supported with GnuTLS 3.5.0 + or OpenSSL 1.1.1 +";
	goto EXPAND_FAILED;
#endif

      /* Line-wrap a string as if it is a header line */

      case EOP_HEADERWRAP:
	{
	unsigned col = 80, lim = 998;
	const uschar * t;

	if (arg)
	  {
	  const uschar * list = arg;
	  int sep = '_';
	  if ((t = string_nextinlist(&list, &sep, NULL, 0)))
	    {
	    col = atoi(CS t);
	    if ((t = string_nextinlist(&list, &sep, NULL, 0)))
	      lim = atoi(CS t);
	    }
	  }
	  if ((t =  wrap_header(sub, col, lim, US"\t", 8)))
	    yield = string_cat(yield, t);
	}
	break;

      /* Convert hex encoding to base64 encoding */

      case EOP_HEX2B64:
	{
	int c = 0;
	int b = -1;
	uschar *in = sub;
	uschar *out = sub;
	uschar *enc;

	for (enc = sub; *enc; enc++)
	  {
	  if (!isxdigit(*enc))
	    {
	    expand_string_message = string_sprintf("%q is not a hex "
	      "string", sub);
	    goto EXPAND_FAILED;
	    }
	  c++;
	  }

	if ((c & 1) != 0)
	  {
	  expand_string_message = string_sprintf("%q contains an odd "
	    "number of characters", sub);
	  goto EXPAND_FAILED;
	  }

	while ((c = *in++) != 0)
	  {
	  if (isdigit(c)) c -= '0';
	  else c = toupper(c) - 'A' + 10;
	  if (b == -1)
	    b = c << 4;
	  else
	    {
	    *out++ = b | c;
	    b = -1;
	    }
	  }

	enc = b64encode(CUS sub, out - sub);
	yield = string_cat(yield, enc);
	break;
	}

      /* Convert octets outside 0x21..0x7E to \xXX form */

      case EOP_HEXQUOTE:
	{
	uschar * t = sub - 1;
	while (*++t)
	  yield = *t < 0x21 || 0x7E < *t
	    ?  string_fmt_append(yield, "\\x%02x", *t)
	    : string_catn(yield, t, 1);
	break;
	}

      /* count the number of list elements */

      case EOP_LISTCOUNT:
	{
	int cnt = 0, sep;
	uschar * buf = store_get(2, sub);

	sep = matchlist_parse_sep(CUSS &sub);
	while (string_nextinlist(CUSS &sub, &sep, buf, 1)) cnt++;
	yield = string_fmt_append(yield, "%d", cnt);
	break;
	}

      /* expand a named list given the name */
      /* handles nested named lists; requotes as colon-sep list */

      case EOP_LISTNAMED:
	expand_string_message = NULL;
	yield = expand_listnamed(yield, sub, arg);
	if (expand_string_message)
	  goto EXPAND_FAILED;
	break;

      /* quote a list-item for the given list-separator */

      /* mask applies a mask to an IP address; for example the result of
      ${mask:131.111.10.206/28} is 131.111.10.192/28. */

      case EOP_MASK:
	{
	int count;
	uschar *endptr;
	int binary[4];
	int type, mask, maskoffset;
	BOOL normalised;
	uschar buffer[64];

	if ((type = string_is_ip_address(sub, &maskoffset)) == 0)
	  {
	  expand_string_message = string_sprintf("%q is not an IP address",
	   sub);
	  goto EXPAND_FAILED;
	  }

	if (maskoffset == 0)
	  {
	  expand_string_message = string_sprintf("missing mask value in %q",
	    sub);
	  goto EXPAND_FAILED;
	  }

	mask = Ustrtol(sub + maskoffset + 1, &endptr, 10);

	if (*endptr || mask < 0 || mask > (type == 4 ? 32 : 128))
	  {
	  expand_string_message = string_sprintf("mask value too big in %q",
	    sub);
	  goto EXPAND_FAILED;
	  }

	/* If an optional 'n' was given, ipv6 gets normalised output:
	colons rather than dots, and zero-compressed. */

	normalised = arg && *arg == 'n';

	/* Convert the address to binary integer(s) and apply the mask */

	sub[maskoffset] = 0;
	count = host_aton(sub, binary);
	host_mask(count, binary, mask);

	/* Convert to masked textual format and add to output. */

	if (type == 4 || !normalised)
	  yield = string_catn(yield, buffer,
	    host_nmtoa(count, binary, mask, buffer, '.'));
	else
	  {
	  ipv6_nmtoa(binary, buffer);
	  yield = string_fmt_append(yield, "%s/%d", buffer, mask);
	  }
	break;
	}

      case EOP_IPV6NORM:
      case EOP_IPV6DENORM:
	{
	int type = string_is_ip_address(sub, NULL);
	int binary[4];
	uschar buffer[44];

	switch (type)
	  {
	  case 6:
	    (void) host_aton(sub, binary);
	    break;

	  case 4:	/* convert to IPv4-mapped IPv6 */
	    binary[0] = binary[1] = 0;
	    binary[2] = 0x0000ffff;
	    (void) host_aton(sub, binary+3);
	    break;

	  case 0:
	    expand_string_message =
	      string_sprintf("%q is not an IP address", sub);
	    goto EXPAND_FAILED;
	  }

	yield = string_catn(yield, buffer, c == EOP_IPV6NORM
		    ? ipv6_nmtoa(binary, buffer)
		    : host_nmtoa(4, binary, -1, buffer, ':')
		  );
	break;
	}

      case EOP_ADDRESS:
      case EOP_LOCAL_PART:
      case EOP_DOMAIN:
	{
	uschar * error;
	int start, end, domain;
	const uschar * t = parse_extract_address(sub, &error, &start, &end,
						&domain, FALSE);
	if (t)
	  if (c != EOP_DOMAIN)
	    yield = c == EOP_LOCAL_PART && domain > 0
	      ? string_catn(yield, t, domain - 1)
	      : string_cat(yield, t);
	  else if (domain > 0)
	    yield = string_cat(yield, t + domain);
	break;
	}

      case EOP_ADDRESSES:
	{
	uschar outsep[2] = { ':', '\0' };
	uschar *address, *error;
	int save_ptr = gstring_length(yield);
	int start, end, domain;  /* Not really used */

	if (Uskip_whitespace(&sub) == '>')
	  if (*outsep = *++sub) ++sub;
	  else
	    {
	    expand_string_message = string_sprintf("output separator "
	      "missing in expanding ${addresses:%s}", --sub);
	    goto EXPAND_FAILED;
	    }
	f.parse_allow_group = TRUE;

	for (;;)
	  {
	  uschar * p = parse_find_address_end(sub, FALSE);
	  uschar saveend = *p;
	  *p = '\0';
	  address = parse_extract_address(sub, &error, &start, &end, &domain,
	    FALSE);
	  *p = saveend;

	  /* Add the address to the output list that we are building. This is
	  done in chunks by searching for the separator character. At the
	  start, unless we are dealing with the first address of the output
	  list, add in a space if the new address begins with the separator
	  character, or is an empty string. */

	  if (address)
	    {
	    if (yield && yield->ptr != save_ptr && address[0] == *outsep)
	      yield = string_catn(yield, US" ", 1);

	    for (;;)
	      {
	      size_t seglen = Ustrcspn(address, outsep);
	      yield = string_catn(yield, address, seglen + 1);

	      /* If we got to the end of the string we output one character
	      too many. */

	      if (address[seglen] == '\0') { yield->ptr--; break; }
	      yield = string_catn(yield, outsep, 1);
	      address += seglen + 1;
	      }

	    /* Output a separator after the string: we will remove the
	    redundant final one at the end. */

	    yield = string_catn(yield, outsep, 1);
	    }

	  if (saveend == '\0') break;
	  sub = p + 1;
	  }

	/* If we have generated anything, remove the redundant final
	separator. */

	if (yield && yield->ptr != save_ptr) yield->ptr--;
	f.parse_allow_group = FALSE;
	break;
	}


      /* quote puts a string in quotes if it is empty or contains anything
      other than alphamerics, underscore, dot, or hyphen.

      quote_local_part puts a string in quotes if RFC 2821/2822 requires it to
      be quoted in order to be a valid local part.

      In both cases, newlines and carriage returns are converted into \n and \r
      respectively */

      case EOP_QUOTE:
      case EOP_QUOTE_LOCAL_PART:
	if (!arg)
	  {
	  BOOL needs_quote = (!*sub);      /* TRUE for empty string */
	  const uschar * t = sub - 1;

	  if (c == EOP_QUOTE)
	    while (!needs_quote && *++t)
	      needs_quote = !isalnum(*t) && !strchr("_-.", *t);

	  else  /* EOP_QUOTE_LOCAL_PART */
	    while (!needs_quote && *++t)
	      needs_quote = !isalnum(*t)
		&& strchr("!#$%&'*+-/=?^_`{|}~", *t) == NULL
		&& (*t != '.' || t == sub || !t[1]);

	  if (needs_quote)
	    {
	    yield = string_catn(yield, US"\"", 1);
	    t = sub - 1;
	    while (*++t)
	      if (*t == '\n')
		yield = string_catn(yield, US"\\n", 2);
	      else if (*t == '\r')
		yield = string_catn(yield, US"\\r", 2);
	      else
		{
		if (*t == '\\' || *t == '"')
		  yield = string_catn(yield, US"\\", 1);
		yield = string_catn(yield, t, 1);
		}
	    yield = string_catn(yield, US"\"", 1);
	    }
	  else
	    yield = string_cat(yield, sub);
	  }

	/* quote_lookuptype does lookup-specific quoting */

	else
	  {
	  const lookup_info * li;
	  uschar * opt = Ustrchr(arg, '_');

	  if (opt) *opt++ = 0;

	  if (!(li = search_findtype(arg, Ustrlen(arg))))
	    {
	    expand_string_message = search_error_message;
	    goto EXPAND_FAILED;
	    }

	  if (li->quote)
	    sub = (li->quote)(sub, opt, li->acq_num);
	  else if (opt)
	    sub = NULL;

	  if (!sub)
	    {
	    expand_string_message = string_sprintf(
	      "%q unrecognized after \"${quote_%s\"",	/*}*/
	      opt, arg);
	    goto EXPAND_FAILED;
	    }

	  yield = string_cat(yield, sub);
	  }
	break;

      /* rx quote sticks in \ before any non-alphameric character so that
      the insertion works in a regular expression. */

      case EOP_RXQUOTE:
	{
	uschar *t = sub - 1;
	while (*(++t) != 0)
	  {
	  if (!isalnum(*t))
	    yield = string_catn(yield, US"\\", 1);
	  yield = string_catn(yield, t, 1);
	  }
	break;
	}

      /* RFC 2047 encodes, assuming headers_charset (default ISO 8859-1) as
      prescribed by the RFC, if there are characters that need to be encoded */

      case EOP_RFC2047:
	yield = string_cat(yield,
			    parse_quote_2047(sub, Ustrlen(sub), headers_charset,
			      FALSE));
	break;

      /* RFC 2047 decode */

      case EOP_RFC2047D:
	{
	int len;
	uschar * error;
	const uschar * decoded = rfc2047_decode(sub, check_rfc2047_length,
					    headers_charset, '?', &len, &error);
	if (error)
	  {
	  expand_string_message = error;
	  goto EXPAND_FAILED;
	  }
	yield = string_catn(yield, decoded, len);
	break;
	}

      /* from_utf8 converts UTF-8 to 8859-1, turning non-existent chars into
      underscores */

      case EOP_FROM_UTF8:
	{
	uschar * buff = store_get(4, sub);
	while (*sub)
	  {
	  int c;
	  GETUTF8INC(c, sub);
	  if (c > 255) c = '_';
	  buff[0] = c;
	  yield = string_catn(yield, buff, 1);
	  }
	break;
	}

      /* replace illegal UTF-8 sequences by replacement character  */

      #define UTF8_REPLACEMENT_CHAR US"?"

      case EOP_UTF8CLEAN:
	{
	int seq_len = 0, index = 0, bytes_left = 0;
	u_long codepoint = (u_long)-1;
	uschar seq_buff[4];			/* accumulate utf-8 here */

	/* Manually track tainting, as we deal in individual chars below */

	if (!yield)
	  yield = string_get_tainted(Ustrlen(sub), sub);
	else if (!yield->s || !yield->ptr)
	  {
	  yield->s = store_get(yield->size = Ustrlen(sub), sub);
	  gstring_reset(yield);
	  }
	else if (is_incompatible(yield->s, sub))
	  gstring_rebuffer(yield, sub);

	/* Check the UTF-8, byte-by-byte */

	while (*sub)
	  {
	  int complete = 0;
	  uschar c = *sub++;

	  if (bytes_left)
	    {
	    if ((c & 0xc0) != 0x80)
		    /* wrong continuation byte; invalidate all bytes */
	      complete = 1; /* error */
	    else
	      {
	      codepoint = (codepoint << 6) | (c & 0x3f);
	      seq_buff[index++] = c;
	      if (--bytes_left == 0)		/* codepoint complete */
		if(codepoint > 0x10FFFF)	/* is it too large? */
		  complete = -1;	/* error (RFC3629 limit) */
		else if ( (codepoint & 0x1FF800 ) == 0xD800 ) /* surrogate */
		  /* A UTF-16 surrogate (which should be one of a pair that
		  encode a Unicode codepoint that is outside the Basic
		  Multilingual Plane).  Error, not UTF8.
		  RFC2279.2 is slightly unclear on this, but 
		  https://unicodebook.readthedocs.io/issues.html#strict-utf8-decoder
		  says "Surrogates characters are also invalid in UTF-8:
		  characters in U+D800—U+DFFF have to be rejected." */
		  complete = -1;
		else
		  {		/* finished; output utf-8 sequence */
		  yield = string_catn(yield, seq_buff, seq_len);
		  index = 0;
		  }
	      }
	    }
	  else	/* no bytes left: new sequence */
	    {
	    if (!(c & 0x80))	/* 1-byte sequence, US-ASCII, keep it */
	      {
	      yield = string_catn(yield, &c, 1);
	      continue;
	      }
	    if ((c & 0xe0) == 0xc0)		/* 2-byte sequence */
	      if (c == 0xc0 || c == 0xc1)	/* 0xc0 and 0xc1 are illegal */
		complete = -1;
	      else
		{
		bytes_left = 1;
		codepoint = c & 0x1f;
		}
	    else if ((c & 0xf0) == 0xe0)		/* 3-byte sequence */
	      {
	      bytes_left = 2;
	      codepoint = c & 0x0f;
	      }
	    else if ((c & 0xf8) == 0xf0)		/* 4-byte sequence */
	      {
	      bytes_left = 3;
	      codepoint = c & 0x07;
	      }
	    else	/* invalid or too long (RFC3629 allows only 4 bytes) */
	      complete = -1;

	    seq_buff[index++] = c;
	    seq_len = bytes_left + 1;
	    }		/* if(bytes_left) */

	  if (complete != 0)
	    {
	    bytes_left = index = 0;
	    yield = string_catn(yield, UTF8_REPLACEMENT_CHAR, 1);
	    }
	  if ((complete == 1) && ((c & 0x80) == 0))
			/* ASCII character follows incomplete sequence */
	      yield = string_catn(yield, &c, 1);
	  }
	/* If given a sequence truncated mid-character, we also want to report ?
	Eg, ${length_1:フィル} is one byte, not one character, so we expect
	${utf8clean:${length_1:フィル}} to yield '?' */

	if (bytes_left != 0)
	  yield = string_catn(yield, UTF8_REPLACEMENT_CHAR, 1);

	break;
	}

#ifdef SUPPORT_I18N
      case EOP_UTF8_DOMAIN_TO_ALABEL:
	{
	uschar * error = NULL;
	const uschar * s = string_domain_utf8_to_alabel(sub, &error);
	if (error)
	  {
	  expand_string_message = string_sprintf(
	    "error converting utf8 (%s) to alabel: %s",
	    string_printing(sub), error);
	  goto EXPAND_FAILED;
	  }
	yield = string_cat(yield, s);
	break;
	}

      case EOP_UTF8_DOMAIN_FROM_ALABEL:
	{
	uschar * error = NULL;
	const uschar * s = string_domain_alabel_to_utf8(sub, &error);
	if (error)
	  {
	  expand_string_message = string_sprintf(
	    "error converting alabel (%s) to utf8: %s",
	    string_printing(sub), error);
	  goto EXPAND_FAILED;
	  }
	yield = string_cat(yield, s);
	break;
	}

      case EOP_UTF8_LOCALPART_TO_ALABEL:
	{
	uschar * error = NULL;
	const uschar * s = string_localpart_utf8_to_alabel(sub, &error);
	if (error)
	  {
	  expand_string_message = string_sprintf(
	    "error converting utf8 (%s) to alabel: %s",
	    string_printing(sub), error);
	  goto EXPAND_FAILED;
	  }
	yield = string_cat(yield, s);
	DEBUG(D_expand) debug_printf_indent("yield: '%Y'\n", yield);
	break;
	}

      case EOP_UTF8_LOCALPART_FROM_ALABEL:
	{
	uschar * error = NULL;
	const uschar * s = string_localpart_alabel_to_utf8(sub, &error);
	if (error)
	  {
	  expand_string_message = string_sprintf(
	    "error converting alabel (%s) to utf8: %s",
	    string_printing(sub), error);
	  goto EXPAND_FAILED;
	  }
	yield = string_cat(yield, s);
	break;
	}
#endif	/* EXPERIMENTAL_INTERNATIONAL */

      /* escape turns all non-printing characters into escape sequences. */

      case EOP_ESCAPE:
	{
	const uschar * t = string_printing(sub);
	yield = string_cat(yield, t);
	break;
	}

      case EOP_ESCAPE8BIT:
	{
	uschar c;

	for (const uschar * s = sub; (c = *s); s++)
	  yield = c < 127 && c != '\\'
	    ? string_catn(yield, s, 1)
	    : string_fmt_append(yield, "\\%03o", c);
	break;
	}

      /* Handle numeric expression evaluation */

      case EOP_EVAL:
      case EOP_EVAL10:
	{
	uschar *save_sub = sub;
	uschar *error = NULL;
	int_eximarith_t n = eval_expr(&sub, (c == EOP_EVAL10), &error, FALSE);
	if (error)
	  {
	  expand_string_message = string_sprintf("error in expression "
	    "evaluation: %s (after processing \"%.*s\")", error,
	    (int)(sub-save_sub), save_sub);
	  goto EXPAND_FAILED;
	  }
	yield = string_fmt_append(yield, PR_EXIM_ARITH, n);
	break;
	}

      /* Handle time period formatting */

      case EOP_TIME_EVAL:
	{
	int n = readconf_readtime(sub, 0, FALSE);
	if (n < 0)
	  {
	  expand_string_message = string_sprintf("string %q is not an "
	    "Exim time interval in %q operator", sub, name);
	  goto EXPAND_FAILED;
	  }
	yield = string_fmt_append(yield, "%d", n);
	break;
	}

      case EOP_TIME_INTERVAL:
	{
	int n;
	const uschar * t = read_number(&n, sub);
	if (*t != 0) /* Not A Number*/
	  {
	  expand_string_message = string_sprintf("string %q is not a "
	    "positive number in %q operator", sub, name);
	  goto EXPAND_FAILED;
	  }
	t = readconf_printtime(n);
	yield = string_cat(yield, t);
	break;
	}

      /* Convert string to base64 encoding */

      case EOP_STR2B64:
      case EOP_BASE64:
	{
#ifndef DISABLE_TLS
	const uschar * s = vp && *(void **)vp->value
	  ? tls_cert_der_b64(*(void **)vp->value)
	  : b64encode(CUS sub, Ustrlen(sub));
#else
	uschar * s = b64encode(CUS sub, Ustrlen(sub));
#endif
	yield = string_cat(yield, s);
	break;
	}

      case EOP_BASE64D:
	{
	uschar * s;
	int len = b64decode(sub, &s, sub);
	if (len < 0)
	  {
	  expand_string_message = string_sprintf("string %q is not "
	    "well-formed for %q operator", sub, name);
	  goto EXPAND_FAILED;
	  }
	yield = string_cat(yield, s);
	break;
	}

      /* strlen returns the length of the string */

      case EOP_STRLEN:
	yield = string_fmt_append(yield, "%d", Ustrlen(sub));
	break;

      /* length_n or l_n takes just the first n characters or the whole string,
      whichever is the shorter;

      substr_m_n, and s_m_n take n characters from offset m; negative m take
      from the end; l_n is synonymous with s_0_n. If n is omitted in substr it
      takes the rest, either to the right or to the left.

      hash_n or h_n makes a hash of length n from the string, yielding n
      characters from the set a-z; hash_n_m makes a hash of length n, but
      uses m characters from the set a-zA-Z0-9.

      nhash_n returns a single number between 0 and n-1 (in text form), while
      nhash_n_m returns a div/mod hash as two numbers "a/b". The first lies
      between 0 and n-1 and the second between 0 and m-1. */

      case EOP_LENGTH:
      case EOP_L:
      case EOP_SUBSTR:
      case EOP_S:
      case EOP_HASH:
      case EOP_H:
      case EOP_NHASH:
      case EOP_NH:
	{
	int sign = 1;
	int value1 = 0;
	int value2 = -1;
	int * pn;
	int len;
	const uschar * ret;

	if (!arg)
	  {
	  expand_string_message = string_sprintf("missing values after %s",
	    name);
	  goto EXPAND_FAILED;
	  }

	/* "length" has only one argument, effectively being synonymous with
	substr_0_n. */

	if (c == EOP_LENGTH || c == EOP_L)
	  {
	  pn = &value2;
	  value2 = 0;
	  }

	/* The others have one or two arguments; for "substr" the first may be
	negative. The second being negative means "not supplied". */

	else
	  {
	  pn = &value1;
	  if (name[0] == 's' && *arg == '-') { sign = -1; arg++; }
	  }

	/* Read up to two numbers, separated by underscores */

	ret = arg;
	while (*arg)
	  {
	  if (arg != ret && *arg == '_' && pn == &value1)
	    {
	    pn = &value2;
	    value2 = 0;
	    if (arg[1]) arg++;
	    }
	  else if (!isdigit(*arg) || INT_MAX/10 - 1 < *pn)
	    {
	    expand_string_message = string_sprintf("%s in %q",
	      !isdigit(*arg) ? "non-digit after underscore" : "value too large",
	      name);
	    goto EXPAND_FAILED;
	    }
	  else
	    *pn = (*pn)*10 + *arg++ - '0';
	  }
	value1 *= sign;

	/* Perform the required operation */

	ret = c == EOP_HASH || c == EOP_H
	  ? compute_hash(sub, value1, value2, &len)
	  : c == EOP_NHASH || c == EOP_NH
	  ? compute_nhash(sub, value1, value2, &len)
	  : extract_substr(sub, value1, value2, &len);
	if (!ret) goto EXPAND_FAILED;

	yield = string_catn(yield, ret, len);
	break;
	}

      /* Stat a path */

      case EOP_STAT:
	{
	uschar smode[12];
	uschar **modetable[3];
	mode_t mode;
	struct stat st;

	if (expand_forbid & RDO_EXISTS)
	  {
	  expand_string_message = US"Use of the stat() expansion is not permitted";
	  goto EXPAND_FAILED;
	  }

	if (stat(CS sub, &st) < 0)
	  {
	  expand_string_message = string_sprintf("stat(%s) failed: %s",
	    sub, strerror(errno));
	  goto EXPAND_FAILED;
	  }
	mode = st.st_mode;
	switch (mode & S_IFMT)
	  {
	  case S_IFIFO: smode[0] = 'p'; break;
	  case S_IFCHR: smode[0] = 'c'; break;
	  case S_IFDIR: smode[0] = 'd'; break;
	  case S_IFBLK: smode[0] = 'b'; break;
	  case S_IFREG: smode[0] = '-'; break;
	  default: smode[0] = '?'; break;
	  }

	modetable[0] = ((mode & 01000) == 0)? mtable_normal : mtable_sticky;
	modetable[1] = ((mode & 02000) == 0)? mtable_normal : mtable_setid;
	modetable[2] = ((mode & 04000) == 0)? mtable_normal : mtable_setid;

	for (int i = 0; i < 3; i++)
	  {
	  memcpy(CS(smode + 7 - i*3), CS(modetable[i][mode & 7]), 3);
	  mode >>= 3;
	  }

	smode[10] = 0;
	yield = string_fmt_append(yield,
	  "mode=%04lo smode=%s inode=%ld device=%ld links=%ld "
	  "uid=%ld gid=%ld size=" OFF_T_FMT " atime=%ld mtime=%ld ctime=%ld",
	  (long)(st.st_mode & 077777), smode, (long)st.st_ino,
	  (long)st.st_dev, (long)st.st_nlink, (long)st.st_uid,
	  (long)st.st_gid, st.st_size, (long)st.st_atime,
	  (long)st.st_mtime, (long)st.st_ctime);
	break;
	}

      /* vaguely random number less than N */

      case EOP_RANDINT:
	{
	int_eximarith_t max = expanded_string_integer(sub, TRUE);

	if (expand_string_message)
	  goto EXPAND_FAILED;
	yield = string_fmt_append(yield, "%d", vaguely_random_number((int)max));
	break;
	}

      /* Reverse IP, including IPv6 to dotted-nibble */

      case EOP_REVERSE_IP:
	{
	int family, maskptr;
	uschar reversed[128];

	family = string_is_ip_address(sub, &maskptr);
	if (family == 0)
	  {
	  expand_string_message = string_sprintf(
	      "reverse_ip() not given an IP address [%s]", sub);
	  goto EXPAND_FAILED;
	  }
	invert_address(reversed, sub);
	yield = string_cat(yield, reversed);
	break;
	}

      case EOP_XTEXTD:
	{
	uschar * s;
	int len = xtextdecode(sub, &s);
	yield = string_catn(yield, s, len);
	break;
	}

      /* Unknown operator */
      default:
	expand_string_message =
	  string_sprintf("unknown expansion operator %q", name);
	goto EXPAND_FAILED;
      }	/* EOP_* switch */

      DEBUG(D_expand)
	{
	const uschar * res = string_from_gstring(yield);
	const uschar * s = res + expansion_start;
	int i = gstring_length(yield) - expansion_start;
	BOOL tainted = is_tainted(s);

	debug_printf_indent("%Vop-res: %.*s\n", "K-----", i, s);
	if (tainted)
	  {
	  debug_printf_indent("%V          %V",
	    flags & ESI_SKIPPING ? "|" : " ",
	    "\\__");
	  debug_print_taint(res);
	  }
	}
       continue;
       }
    }

  /* Not an item or an operator */
  /* Handle a plain name. If this is the first thing in the expansion, release
  the pre-allocated buffer. If the result data is known to be in a new buffer,
  newsize will be set to the size of that buffer, and we can just point at that
  store instead of copying. Many expansion strings contain just one reference,
  so this is a useful optimization, especially for humungous headers
  ($message_headers). */
						/*{*/
  if (*s++ == '}')
    {
    const uschar * value;
    int len;
    int newsize = 0;
    gstring * g = NULL;

    if (!yield)
      g = store_get(sizeof(gstring), GET_UNTAINTED);
    else if (yield->ptr == 0)
      {
      if (resetok) reset_point = store_reset(reset_point);
      yield = NULL;
      reset_point = store_mark();
      g = store_get(sizeof(gstring), GET_UNTAINTED);	/* alloc _before_ calling find_variable() */
      }
    if (!(value = find_variable(name, flags, &newsize)))
      {
      expand_string_message =
        string_sprintf("unknown variable in \"${%s}\"", name);
      check_variable_error_message(name);
      goto EXPAND_FAILED;
      }
    len = Ustrlen(value);
    if (!yield && newsize)
      {
      yield = g;
      yield->size = newsize;
      yield->ptr = len;
      yield->s = US value; /* known to be in new store i.e. a copy, so deconst safe */
      }
    else
      yield = string_catn(yield, value, len);
    continue;
    }

  /* Else there's something wrong */

  expand_string_message =
    string_sprintf("\"${%s\" is not a known operator (or a } is missing "
    "in a variable reference)", name);
  goto EXPAND_FAILED;
  }

/* If we hit the end of the string when brace_ends is set, there is a missing
terminating brace. */

if (flags & ESI_BRACE_ENDS && !*s)
  {							/*{{*/
  expand_string_message = malformed_header
    ? US"missing } at end of string - could be header name not terminated by colon"
    : US"missing } at end of string";
  goto EXPAND_FAILED;
  }

/* Expansion succeeded; yield may still be NULL here if nothing was actually
added to the string. If so, set up an empty string. Add a terminating zero. If
left != NULL, return a pointer to the terminator. */

 {
  uschar * res;

  if (!yield)
    yield = string_get(1);
  res = string_from_gstring(yield);
  if (left) *left = s;

  /* Any stacking store that was used above the final string is no longer needed.
  In many cases the final string will be the first one that was got and so there
  will be optimal store usage. */

  if (resetok) gstring_release_unused(yield);
  else if (resetok_p) *resetok_p = FALSE;

  DEBUG(D_expand)
    {
    BOOL tainted = is_tainted(res);
    debug_printf_indent("%Vexpanded: %.*W\n",
      "K---",
      (int)(s - orig_string), orig_string);
    debug_printf_indent("%Vresult: ",
      flags & ESI_SKIPPING ? "K-----" : "\\_____");
    if (*res || !(flags & ESI_SKIPPING))
      debug_printf("%W\n", res);
    else
      debug_printf(" %Vskipped%V\n", "<", ">");
    if (tainted)
      {
      debug_printf_indent("%V          %V",
	flags & ESI_SKIPPING ? "|" : " ",
	"\\__"
	);
      debug_print_taint(res);
      }
    if (flags & ESI_SKIPPING)
      debug_printf_indent("%Vskipping: result is not used\n", "\\___");
    }
  if (textonly_p) *textonly_p = textonly;
  expand_level--;
  return res;
 }

/* This is the failure exit: easiest to program with a goto. We still need
to update the pointer to the terminator, for cases of nested calls with "fail".
*/

EXPAND_FAILED_CURLY:
if (malformed_header)
  expand_string_message =
    US"missing or misplaced { or } - could be header name not terminated by colon";

else if (!expand_string_message || !*expand_string_message)
  expand_string_message = US"missing or misplaced { or }";

/* At one point, Exim reset the store to yield (if yield was not NULL), but
that is a bad idea, because expand_string_message is in dynamic store. */

EXPAND_FAILED:
if (left) *left = s;
DEBUG(D_expand)
  {
  debug_printf_indent("%Vfailed to expand: %s\n", "K", orig_string);
  debug_printf_indent("%Verror message: %s\n",
    f.expand_string_forcedfail ? "K---" : "\\___", expand_string_message);
  if (f.expand_string_forcedfail)
    debug_printf_indent("%Vfailure was forced\n", "\\");
  }
if (resetok_p && !resetok) *resetok_p = FALSE;
expand_level--;
return NULL;
}



/* This is the external function call. Do a quick check for any expansion
metacharacters, and if there are none, just return the input string.

Arguments
	the string to be expanded
	optional pointer for return boolean indicating no-dynamic-expansions

Returns:  the expanded string, or NULL if expansion failed; if failure was
          due to a lookup deferring, search_find_defer will be TRUE
*/

const uschar *
expand_string_2(const uschar * string, BOOL * textonly_p)
{
f.expand_string_forcedfail = f.search_find_defer = malformed_header = FALSE;
if (Ustrpbrk(string, "$\\") != NULL)
  {
  int old_pool = store_pool;
  uschar * s;

  store_pool = POOL_MAIN;
    s = expand_string_internal(string, ESI_HONOR_DOLLAR, NULL, NULL, textonly_p);
  store_pool = old_pool;
  return s;
  }
if (textonly_p) *textonly_p = TRUE;
return string;
}


/* Just return whether the string is non-empty after expansion */

BOOL
expand_string_nonempty(const uschar * string)
{
const uschar * s;
if (!string) return FALSE;
s = expand_string_internal(string, ESI_HONOR_DOLLAR | ESI_EXISTS_ONLY,
			    NULL, NULL, NULL);
return s && *s;
}




/*************************************************
*              Expand and copy                   *
*************************************************/

/* Now and again we want to expand a string and be sure that the result is in a
new bit of store. This function does that.
Since we know it has been copied, the de-const cast is safe.

Argument: the string to be expanded
Returns:  the expanded string, always in a new bit of store, or NULL
*/

uschar *
expand_string_copy(const uschar * string)
{
const uschar * yield = expand_string(string);
if (yield == string) yield = string_copy(string);
return US yield;
}



/*************************************************
*        Expand and interpret as an integer      *
*************************************************/

/* Expand a string, and convert the result into an integer.

Arguments:
  string  the string to be expanded
  isplus  TRUE if a non-negative number is expected

Returns:  the integer value, or
          -1 for an expansion error               ) in both cases, message in
          -2 for an integer interpretation error  ) expand_string_message
          expand_string_message is set NULL for an OK integer
*/

int_eximarith_t
expand_string_integer(uschar *string, BOOL isplus)
{
return expanded_string_integer(expand_string(string), isplus);
}


/*************************************************
 *         Interpret string as an integer        *
 *************************************************/

/* Convert a string (that has already been expanded) into an integer.

This function is used inside the expansion code.

Arguments:
  s       the string to be expanded
  isplus  TRUE if a non-negative number is expected

Returns:  the integer value, or
          -1 if string is NULL (which implies an expansion error)
          -2 for an integer interpretation error
          expand_string_message is set NULL for an OK integer
*/

static int_eximarith_t
expanded_string_integer(const uschar *s, BOOL isplus)
{
int_eximarith_t value;
uschar *msg = US"invalid integer %q";
uschar *endptr;

/* If expansion failed, expand_string_message will be set. */

if (!s) return -1;

/* On an overflow, strtol() returns LONG_MAX or LONG_MIN, and sets errno
to ERANGE. When there isn't an overflow, errno is not changed, at least on some
systems, so we set it zero ourselves. */

errno = 0;
expand_string_message = NULL;               /* Indicates no error */

/* Before Exim 4.64, strings consisting entirely of whitespace compared
equal to 0.  Unfortunately, people actually relied upon that, so preserve
the behaviour explicitly.  Stripping leading whitespace is a harmless
noop change since strtol skips it anyway (provided that there is a number
to find at all). */
if (isspace(*s))
  if (Uskip_whitespace(&s) == '\0')
    {
      DEBUG(D_expand)
       debug_printf_indent("treating blank string as number 0\n");
      return 0;
    }

value = strtoll(CS s, CSS &endptr, 10);

if (endptr == s)
  msg = US"integer expected but %q found";
else if (value < 0 && isplus)
  msg = US"non-negative integer expected but %q found";
else
  {
  switch (tolower(*endptr))
    {
    default:
      break;
    case 'k':
      if (value > EXIM_ARITH_MAX/1024 || value < EXIM_ARITH_MIN/1024) errno = ERANGE;
      else value *= 1024;
      endptr++;
      break;
    case 'm':
      if (value > EXIM_ARITH_MAX/(1024*1024) || value < EXIM_ARITH_MIN/(1024*1024)) errno = ERANGE;
      else value *= 1024*1024;
      endptr++;
      break;
    case 'g':
      if (value > EXIM_ARITH_MAX/(1024*1024*1024) || value < EXIM_ARITH_MIN/(1024*1024*1024)) errno = ERANGE;
      else value *= 1024*1024*1024;
      endptr++;
      break;
    }
  if (errno == ERANGE)
    msg = US"absolute value of integer %q is too large (overflow)";
  else
    if (Uskip_whitespace(&endptr) == 0) return value;
  }

expand_string_message = string_sprintf(CS msg, s);
return -2;
}


/* These values are usually fixed boolean values, but they are permitted to be
expanded strings.

Arguments:
  addr       address being routed
  mtype      the module type
  mname      the module name
  dbg_opt    debug selectors
  oname      the option name
  bvalue     the router's boolean value
  svalue     the router's string value
  rvalue     where to put the returned value

Returns:     OK     value placed in rvalue
             DEFER  expansion failed
*/

int
exp_bool(address_item * addr,
  const uschar * mtype, const uschar * mname, unsigned dbg_opt,
  uschar * oname, BOOL bvalue,
  const uschar * svalue, BOOL * rvalue)
{
const uschar * expanded;

DEBUG(D_expand) debug_printf_indent("try option %s\n", oname);
if (!svalue) { *rvalue = bvalue; return OK; }

if (!(expanded = expand_string(svalue)))
  {
  if (f.expand_string_forcedfail)
    {
    DEBUG(dbg_opt) debug_printf("expansion of %q forced failure\n", oname);
    *rvalue = bvalue;
    return OK;
    }
  addr->message = string_sprintf("failed to expand %q in %s %s: %s",
      oname, mname, mtype, expand_string_message);
  DEBUG(dbg_opt) debug_printf("%s\n", addr->message);
  return DEFER;
  }

DEBUG(dbg_opt) debug_printf("expansion of %q yields %q\n", oname,
  expanded);

if (strcmpic(expanded, US"true") == 0 || strcmpic(expanded, US"yes") == 0)
  *rvalue = TRUE;
else if (strcmpic(expanded, US"false") == 0 || strcmpic(expanded, US"no") == 0)
  *rvalue = FALSE;
else
  {
  addr->message = string_sprintf("%q is not a valid value for the "
    "%q option in the %s %s", expanded, oname, mname, mtype);
  return DEFER;
  }

return OK;
}



/* Avoid potentially exposing a password in a string about to be logged */

uschar *
expand_hide_passwords(uschar * s)
{
return (  (  Ustrstr(s, "failed to expand") != NULL
	  || Ustrstr(s, "expansion of ")    != NULL
	  )
       && (  Ustrstr(s, "mysql")   != NULL
	  || Ustrstr(s, "pgsql")   != NULL
	  || Ustrstr(s, "redis")   != NULL
	  || Ustrstr(s, "sqlite")  != NULL
	  || Ustrstr(s, "ldap:")   != NULL
	  || Ustrstr(s, "ldaps:")  != NULL
	  || Ustrstr(s, "ldapi:")  != NULL
	  || Ustrstr(s, "ldapdn:") != NULL
	  || Ustrstr(s, "ldapm:")  != NULL
       )  )
  ? US"Temporary internal error" : s;
}


/* Read given named file into big_buffer.  Use for keying material etc.
The content will have an ascii NUL appended.

Arguments:
 filename	as it says

Return:  pointer to buffer, or NULL on error.
*/

uschar *
expand_file_big_buffer(const uschar * filename)
{
int fd, off = 0, len;

if ((fd = exim_open2(CS filename, O_RDONLY)) < 0)
  {
  log_write(0, LOG_MAIN | LOG_PANIC, "unable to open file '%s' for reading: %s",
	     filename, strerror(errno));
  return NULL;
  }

do
  {
  if ((len = read(fd, big_buffer + off, big_buffer_size - 2 - off)) < 0)
    {
    (void) close(fd);
    log_write(0, LOG_MAIN|LOG_PANIC, "unable to read file: %s", filename);
    return NULL;
    }
  off += len;
  }
while (len > 0);

(void) close(fd);
big_buffer[off] = '\0';
return big_buffer;
}



/*************************************************
* Error-checking for testsuite                   *
*************************************************/
typedef struct {
  uschar * 	region_start;
  uschar *	region_end;
  const uschar *var_name;
  const uschar *var_data;
} err_ctx;

/* Called via tree_walk, which allows nonconst name/data.  Our usage is const. */
static void
assert_variable_notin(uschar * var_name, uschar * var_data, void * ctx)
{
err_ctx * e = ctx;
if (var_data >= e->region_start  &&  var_data < e->region_end)
  {
  e->var_name = CUS var_name;
  e->var_data = CUS var_data;
  }
}

void
assert_no_variables(void * ptr, int len, const char * filename, int linenumber)
{
err_ctx e = { .region_start = ptr, .region_end = US ptr + len,
	      .var_name = NULL, .var_data = NULL };

/* check acl_ variables */
tree_walk(acl_var_c, assert_variable_notin, &e);
tree_walk(acl_var_m, assert_variable_notin, &e);

/* check auth<n> variables.
assert_variable_notin() treats as const, so deconst is safe. */
for (int i = 0; i < AUTH_VARS; i++) if (auth_vars[i])
  assert_variable_notin(US"auth<n>", US auth_vars[i], &e);

#ifdef WITH_CONTENT_SCAN
/* check regex<n> variables. assert_variable_notin() treats as const. */
for (int i = 0; i < REGEX_VARS; i++) if (regex_vars[i])
  assert_variable_notin(US"regex<n>", US regex_vars[i], &e);
#endif

/* check known-name variables */
for (var_entry * v = var_table; v < var_table + nelem(var_table); v++)
  if (v->type == vtype_stringptr)
    assert_variable_notin(US v->name, *(USS v->value), &e);

/* check dns and address trees */
tree_walk(tree_dns_fails,     assert_variable_notin, &e);
tree_walk(tree_duplicates,    assert_variable_notin, &e);
tree_walk(tree_nonrecipients, assert_variable_notin, &e);
tree_walk(tree_unusable,      assert_variable_notin, &e);

if (e.var_name)
  log_write_die(0, LOG_MAIN,
    "live variable '%s' destroyed by reset_store at %s:%d\n- value '%.64s'",
    e.var_name, filename, linenumber, e.var_data);
}



/*************************************************
**************************************************
*             Stand-alone test program           *
**************************************************
*************************************************/

#ifdef STAND_ALONE


BOOL
regex_match_and_setup(const pcre2_code *re, uschar *subject, int options, int setup)
{
int ovec[3*(EXPAND_MAXN+1)];
int n = pcre_exec(re, NULL, subject, Ustrlen(subject), 0, PCRE_EOPT|options,
  ovec, nelem(ovec));
BOOL yield = n >= 0;
if (n == 0) n = EXPAND_MAXN + 1;
if (yield)
  {
  expand_nmax = setup < 0 ? 0 : setup + 1;
  for (int nn = setup < 0 ? 0 : 2; nn < n*2; nn += 2)
    {
    expand_nstring[expand_nmax] = subject + ovec[nn];
    expand_nlength[expand_nmax++] = ovec[nn+1] - ovec[nn];
    }
  expand_nmax--;
  }
return yield;
}


int main(int argc, uschar **argv)
{
uschar buffer[1024];

debug_selector = D_v;
debug_file = stderr;
debug_fd = fileno(debug_file);
big_buffer = malloc(big_buffer_size);
store_init();

for (int i = 1; i < argc; i++)
  {
  if (argv[i][0] == '+')
    {
    debug_trace_memory = 2;
    argv[i]++;
    }
  if (isdigit(argv[i][0]))
    debug_selector = Ustrtol(argv[i], NULL, 0);
  else
    if (Ustrspn(argv[i], "abcdefghijklmnopqrtsuvwxyz0123456789-.:/") ==
        Ustrlen(argv[i]))
      {
#ifdef LOOKUP_LDAP
      eldap_default_servers = argv[i];
#endif
#ifdef LOOKUP_MYSQL
      mysql_servers = argv[i];
#endif
#ifdef LOOKUP_PGSQL
      pgsql_servers = argv[i];
#endif
#ifdef LOOKUP_REDIS
      redis_servers = argv[i];
#endif
      }
#ifdef EXIM_PERL
  else opt_perl_startup = argv[i];
#endif
  }

printf("Testing string expansion: debug_level = %d\n\n", debug_level);

expand_nstring[1] = US"string 1....";
expand_nlength[1] = 8;
expand_nmax = 1;

#ifdef EXIM_PERL
if (opt_perl_startup != NULL)
  {
  uschar *errstr;
  printf("Starting Perl interpreter\n");
  errstr = init_perl(opt_perl_startup);
  if (errstr)
    {
    printf("** error in perl_startup code: %s\n", errstr);
    return EXIT_FAILURE;
    }
  }
#endif /* EXIM_PERL */

/* Thie deliberately regards the input as untainted, so that it can be
expanded; only reasonable since this is a test for string-expansions. */

while (fgets(buffer, sizeof(buffer), stdin) != NULL)
  {
  rmark reset_point = store_mark();
  uschar *yield = expand_string(buffer);
  if (yield)
    printf("%s\n", yield);
  else
    {
    if (f.search_find_defer) printf("search_find deferred\n");
    printf("Failed: %s\n", expand_string_message);
    if (f.expand_string_forcedfail) printf("Forced failure\n");
    printf("\n");
    }
  store_reset(reset_point);
  }

search_tidyup();

return 0;
}

#endif	/*STAND_ALONE*/

#endif	/*!MACRO_PREDEF*/
/* vi: aw ai sw=2
*/
/* End of expand.c */
