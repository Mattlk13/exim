/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) The Exim Maintainers 2020 - 2024 */
/* Copyright (c) University of Cambridge 1995 - 2018 */
/* See the file NOTICE for conditions of use and distribution. */
/* SPDX-License-Identifier: GPL-2.0-or-later */


/* A small freestanding program to build dbm databases from serial input. For
alias files, this program fulfils the function of the newaliases program used
by other mailers, but it can be used for other dbm data files too. It operates
by writing a new file or files, and then renaming; otherwise old entries can
never get flushed out.

This program is clever enough to cope with ndbm, which creates two files called
<name>.dir and <name>.pag, or with db, which creates a single file called
<name>.db. If native db is in use (USE_DB defined) or tdb is in use (USE_TDB
defined) there is no extension to the output filename. This is also handled. If
there are any other variants, the program won't cope.

The first argument to the program is the name of the serial file; the second
is the base name for the DBM file(s). When native db is in use, these must be
different.

Input lines beginning with # are ignored, as are blank lines. Entries begin
with a key terminated by a colon or end of line or whitespace and continue with
indented lines. Keys may be quoted if they contain colons or whitespace or #
characters. */


#include "exim.h"
#include "hintsdb.h"

uschar * spool_directory = NULL;	/* dummy for hintsdb.h */

/******************************************************************************/
					/* dummies needed by Solaris build */
void
millisleep(int msec)
{}
uschar *
readconf_printtime(int t)
{ return NULL; }
const uschar * expand_string_2(const uschar * string, BOOL * textonly_p)
{return NULL; }
void *
store_get_3(int size, const void * proto_mem, const char *filename, int linenumber)
{ return NULL; }
void **
store_reset_3(void **ptr, const char *filename, int linenumber)
{ return NULL; }
void
store_release_above_3(void *ptr, const char *func, int linenumber)
{ }
gstring *
string_catn(gstring * g, const uschar * s, int count)
{ return NULL; }
gstring *
string_vformat_trc(gstring * g, const uschar * func, unsigned line,
  unsigned size_limit, unsigned flags, const char *format, va_list ap)
{ return NULL; }
uschar *
string_sprintf_trc(const char * a, const uschar * b, unsigned c, ...)
{ return NULL; }
BOOL
string_format_trc(uschar * buf, int len, const uschar * func, unsigned line,
  const char * fmt, ...)
{ return FALSE; }
void
log_write(unsigned int selector, int flags, const char *format, ...)
{ }


struct global_flags	f;
unsigned int		log_selector[1];
uschar *		queue_name;
BOOL			split_spool_directory;


/******************************************************************************/


#define max_insize   20000
#define max_outsize 100000

/* This is global because it's defined in the headers and compilers grumble
if it is made static. */

const uschar *hex_digits = CUS"0123456789abcdef";


/*******************
*   Debug output   *
*******************/

unsigned int debug_selector = 0;	/* set -1 for debugging */

void
debug_printf(const char * fmt, ...)
{
va_list ap;
va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
void
debug_printf_indent(const char * fmt, ...)
{
va_list ap;
va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}



#ifdef STRERROR_FROM_ERRLIST
/* Some old-fashioned systems still around (e.g. SunOS4) don't have strerror()
in their libraries, but can provide the same facility by this simple
alternative function. */

char *
strerror(int n)
{
if (n < 0 || n >= sys_nerr) return "unknown error number";
return sys_errlist[n];
}
#endif /* STRERROR_FROM_ERRLIST */



/*************************************************
*          Interpret escape sequence             *
*************************************************/

/* This function is copied from the main Exim code.

Arguments:
  pp       points a pointer to the initiating "\" in the string;
           the pointer gets updated to point to the final character
Returns:   the value of the character escape
*/

int
string_interpret_escape(const uschar **pp)
{
int ch;
const uschar *p = *pp;
ch = *(++p);
if (ch == '\0') return **pp;
if (isdigit(ch) && ch != '8' && ch != '9')
  {
  ch -= '0';
  if (isdigit(p[1]) && p[1] != '8' && p[1] != '9')
    {
    ch = ch * 8 + *(++p) - '0';
    if (isdigit(p[1]) && p[1] != '8' && p[1] != '9')
      ch = ch * 8 + *(++p) - '0';
    }
  }
else switch(ch)
  {
  case 'n':  ch = '\n'; break;
  case 'r':  ch = '\r'; break;
  case 't':  ch = '\t'; break;
  case 'x':
  ch = 0;
  if (isxdigit(p[1]))
    {
    ch = ch * 16 +
      Ustrchr(hex_digits, tolower(*(++p))) - hex_digits;
    if (isxdigit(p[1])) ch = ch * 16 +
      Ustrchr(hex_digits, tolower(*(++p))) - hex_digits;
    }
  break;
  }
*pp = p;
return ch;
}


/*************************************************
*               Main Program                     *
*************************************************/

int main(int argc, char **argv)
{
int started;
int count = 0;
int dupcount = 0;
int yield = 0;
int arg = 1;
int add_zero = 1;
BOOL lowercase = TRUE;
BOOL warn = TRUE;
BOOL duperr = TRUE;
BOOL lastdup = FALSE;
#if !defined (USE_DB) && !defined(USE_TDB) && !defined(USE_GDBM) && !defined(USE_SQLITE)
int is_db = 0;
struct stat statbuf;
#endif
FILE *f;
EXIM_DB *d;
EXIM_DATUM key, content;
uschar *bptr;
uschar  keybuffer[256];
uschar  temp_dbmname[512];
uschar  real_dbmname[512];
uschar  dirname[512];
uschar *buffer = malloc(max_outsize);
uschar *line = malloc(max_insize);

while (argc > 1)
  {
  if      (Ustrcmp(argv[arg], "-nolc") == 0)     lowercase = FALSE;
  else if (Ustrcmp(argv[arg], "-nowarn") == 0)   warn = FALSE;
  else if (Ustrcmp(argv[arg], "-lastdup") == 0)  lastdup = TRUE;
  else if (Ustrcmp(argv[arg], "-noduperr") == 0) duperr = FALSE;
  else if (Ustrcmp(argv[arg], "-nozero") == 0)   add_zero = 0;
  else break;
  arg++;
  argc--;
  }

if (argc != 3)
  {
  printf("usage: exim_dbmbuild [-nolc] <source file> <dbm base name>\n");
  exit(EXIT_FAILURE);
  }

if (Ustrcmp(argv[arg], "-") == 0)
  f = stdin;
else if (!(f = fopen(argv[arg], "rb")))
  {
  printf("exim_dbmbuild: unable to open %s: %s\n", argv[arg], strerror(errno));
  exit(EXIT_FAILURE);
  }

/* By default Berkeley db does not put extensions on... which
can be painful! */

#if defined(USE_DB) || defined(USE_TDB) || defined(USE_GDBM) && !defined(USE_SQLITE)
if (Ustrcmp(argv[arg], argv[arg+1]) == 0)
  {
  printf("exim_dbmbuild: input and output filenames are the same\n");
  exit(EXIT_FAILURE);
  }
#endif

/* Check length of filename; allow for adding .dbmbuild_temp and .db or
.dir/.pag later. */

if (strlen(argv[arg+1]) > sizeof(temp_dbmname) - 20)
  {
  printf("exim_dbmbuild: output filename is ridiculously long\n");
  exit(EXIT_FAILURE);
  }

Ustrcpy(temp_dbmname, US argv[arg+1]);
Ustrcat(temp_dbmname, US".dbmbuild_temp");

Ustrcpy(dirname, temp_dbmname);
if ((bptr = Ustrrchr(dirname, '/')))
  *bptr = '\0';
else
  Ustrcpy(dirname, US".");

/* It is apparently necessary to open with O_RDWR for this to work
with gdbm-1.7.3, though no reading is actually going to be done. */

if (!(d = exim_dbopen(temp_dbmname, dirname, O_RDWR|O_CREAT|O_EXCL, 0644)))
  {
  printf("exim_dbmbuild: unable to create %s: %s\n", temp_dbmname,
    strerror(errno));
  (void)fclose(f);
  exit(EXIT_FAILURE);
  }

/* Unless using native db calls, see if we have created <name>.db; if not,
assume .dir & .pag */

#if !defined(USE_DB) && !defined(USE_TDB) && !defined(USE_GDBM) && !defined(USE_SQLITE)
sprintf(CS real_dbmname, "%s.db", temp_dbmname);
is_db = Ustat(real_dbmname, &statbuf) == 0;
#endif

/* Now do the business */

bptr = buffer;
started = 0;

while (Ufgets(line, max_insize, f) != NULL)
  {
  uschar *p;
  int len = Ustrlen(line);

  p = line + len;

  if (len >= max_insize - 1 && p[-1] != '\n')
    {
    printf("Overlong line read: max permitted length is %d\n", max_insize - 1);
    yield = 2;
    goto TIDYUP;
    }

  if (line[0] == '#') continue;
  while (p > line && isspace(p[-1])) p--;
  *p = 0;
  if (line[0] == 0) continue;

  /* A continuation line is valid only if there was a previous first
  line. */

  if (isspace(line[0]))
    {
    uschar *s = line;
    if (!started)
      {
      printf("Unexpected continuation line ignored\n%s\n\n", line);
      continue;
      }
    while (isspace(*s)) s++;
    *(--s) = ' ';

    if (bptr - buffer + p - s >= max_outsize - 1)
      {
      printf("Continued set of lines is too long: max permitted length is %d\n",
        max_outsize -1);
      yield = 2;
      goto TIDYUP;
      }

    Ustrcpy(bptr, s);
    bptr += p - s;
    }

  /* A first line must have a name followed by a colon or whitespace or
  end of line, but first finish with a previous line. The key is lower
  cased by default - this is what the newaliases program for sendmail does.
  However, there's an option not to do this. */

  else
    {
    int i, rc;
    uschar * s = line;
    const uschar * keystart;

    if (started)
      {
      exim_datum_init(&content);
      exim_datum_data_set(&content, buffer);
      exim_datum_size_set(&content, bptr - buffer + add_zero);

      rc = exim_dbputb(d, &key, &content);
      switch(rc)
        {
        case EXIM_DBPUTB_OK:
	  count++;
	  break;

        case EXIM_DBPUTB_DUP:
	  if (warn) fprintf(stderr, "** Duplicate key \"%s\"\n", keybuffer);
	  dupcount++;
	  if(duperr) yield = 1;
	  if (lastdup) exim_dbput(d, &key, &content);
	  break;

        default:
	  fprintf(stderr, "Error %d while writing key %s: errno=%d\n", rc,
	    keybuffer, errno);
	  yield = 2;
	  goto TIDYUP;
        }

      bptr = buffer;
      }

    exim_datum_init(&key);
    exim_datum_data_set(&key, keybuffer);

    /* Deal with quoted keys. Escape sequences always make one character
    out of several, so we can re-build in place. */

    if (*s == '\"')
      {
      uschar * t = s++;
      keystart = t;
      while (*s && *s != '\"')
        {
	*t++ = *s == '\\'
	? string_interpret_escape((const uschar **)&s)
	: *s;
        s++;
        }
      if (*s) s++;               /* Past terminating " */
      exim_datum_size_set(&key, t - keystart + add_zero);
      }
    else
      {
      keystart = s;
      while (*s && *s != ':' && !isspace(*s)) s++;
      exim_datum_size_set(&key, s - keystart + add_zero);
      }

    if (exim_datum_size_get(&key) > 256)
      {
      printf("Keys longer than 255 characters cannot be handled\n");
      started = 0;
      yield = 2;
      goto TIDYUP;
      }

    if (lowercase)
      for (i = 0; i < exim_datum_size_get(&key) - add_zero; i++)
        keybuffer[i] = tolower(keystart[i]);
    else
      for (i = 0; i < exim_datum_size_get(&key) - add_zero; i++)
        keybuffer[i] = keystart[i];

    keybuffer[i] = 0;
    started = 1;

    while (isspace(*s)) s++;
    if (*s == ':')
      {
      s++;
      while (isspace(*s)) s++;
      }
    if (*s)
      {
      Ustrcpy(bptr, s);
      bptr += p - s;
      }
    else buffer[0] = 0;
    }
  }

if (started)
  {
  int rc;
  exim_datum_init(&content);
  exim_datum_data_set(&content, buffer);
  exim_datum_size_set(&content, bptr - buffer + add_zero);

  rc = exim_dbputb(d, &key, &content);
  switch(rc)
    {
    case EXIM_DBPUTB_OK:
    count++;
    break;

    case EXIM_DBPUTB_DUP:
    if (warn) fprintf(stderr, "** Duplicate key \"%s\"\n", keybuffer);
    dupcount++;
    if (duperr) yield = 1;
    if (lastdup) exim_dbput(d, &key, &content);
    break;

    default:
    fprintf(stderr, "Error %d while writing key %s: errno=%d\n", rc,
      keybuffer, errno);
    yield = 2;
    break;
    }
  }

/* Close files, rename or abandon the temporary files, and exit */

TIDYUP:

exim_dbclose(d);
(void)fclose(f);

/* If successful, output the number of entries and rename the temporary
files. */

if (yield == 0 || yield == 1)
  {
  printf("%d entr%s written\n", count, (count == 1)? "y" : "ies");
  if (dupcount > 0)
    {
    printf("%d duplicate key%s \n", dupcount, (dupcount > 1)? "s" : "");
    }

#if defined(USE_DB) || defined(USE_TDB) || defined(USE_GDBM) || defined(USE_SQLITE)
  Ustrcpy(real_dbmname, temp_dbmname);
  Ustrcpy(buffer, US argv[arg+1]);
  if (Urename(real_dbmname, buffer) != 0)
    {
    printf("Unable to rename %s as %s\n", real_dbmname, buffer);
    return 1;
    }
#else

  /* Rename a single .db file */

  if (is_db)
    {
    sprintf(CS real_dbmname, "%s.db", temp_dbmname);
    sprintf(CS buffer, "%s.db", argv[arg+1]);
    if (Urename(real_dbmname, buffer) != 0)
      {
      printf("Unable to rename %s as %s\n", real_dbmname, buffer);
      return 1;
      }
    }

  /* Rename .dir and .pag files */

  else
    {
    sprintf(CS real_dbmname, "%s.dir", temp_dbmname);
    sprintf(CS buffer, "%s.dir", argv[arg+1]);
    if (Urename(real_dbmname, buffer) != 0)
      {
      printf("Unable to rename %s as %s\n", real_dbmname, buffer);
      return 1;
      }

    sprintf(CS real_dbmname, "%s.pag", temp_dbmname);
    sprintf(CS buffer, "%s.pag", argv[arg+1]);
    if (Urename(real_dbmname, buffer) != 0)
      {
      printf("Unable to rename %s as %s\n", real_dbmname, buffer);
      return 1;
      }
    }

#endif /* USE_DB || USE_TDB || USE_GDBM || USE_SQLITE */
  }

/* Otherwise unlink the temporary files. */

else
  {
  printf("dbmbuild abandoned\n");
#if defined(USE_DB) || defined(USE_TDB) || defined(USE_GDBM) || defined(USE_SQLITE)
  /* We created it, so safe to delete despite the name coming from outside */
  /* coverity[tainted_string] */
  Uunlink(temp_dbmname);
#else
  if (is_db)
    {
    sprintf(CS real_dbmname, "%s.db", temp_dbmname);
    Uunlink(real_dbmname);
    }
  else
    {
    sprintf(CS real_dbmname, "%s.dir", temp_dbmname);
    Uunlink(real_dbmname);
    sprintf(CS real_dbmname, "%s.pag", temp_dbmname);
    Uunlink(real_dbmname);
    }
#endif /* USE_DB || USE_TDB || USE_GDBM || USE_SQLITE */
  }

return yield;
}

/* End of exim_dbmbuild.c */
/* se aw ai sw=2
*/
