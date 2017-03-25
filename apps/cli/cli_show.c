/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * 
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif 
#include <dirent.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pwd.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* Exported functions in this file are in clixon_cli_api.h */
#include "clixon_cli_api.h"
#include "cli_common.h" /* internal functions */

static int xml2csv(FILE *f, cxobj *x, cvec *cvv);
//static int xml2csv_raw(FILE *f, cxobj *x);

/*! Completion callback intended for automatically generated data model
 *
 * Returns an expand-type list of commands as used by cligen 'expand' 
 * functionality.
 *
 * Assume callback given in a cligen spec: a <x:int expand_dbvar("arg")
 * @param[in]   h        clicon handle 
 * @param[in]   name     Name of this function (eg "expand_dbvar")
 * @param[in]   cvv      The command so far. Eg: cvec [0]:"a 5 b"; [1]: x=5;
 * @param[in]   argv     Arguments given at the callback ("<db>" "<xmlkeyfmt>")
 * @param[out]  len      len of return commands & helptxt 
 * @param[out]  commands vector of function pointers to callback functions
 * @param[out]  helptxt  vector of pointers to helptexts
 * @see cli_expand_var_generate  This is where arg is generated
 * XXX: helptexts?
 */
int
expandv_dbvar(void   *h, 
	      char   *name, 
	      cvec   *cvv, 
	      cvec   *argv, 
	      cvec  *commands,
	      cvec  *helptexts)
{
    int              retval = -1;
    char            *xkfmt;
    char            *dbstr;    
    cxobj           *xt = NULL;
    char            *xkpath = NULL;
    cxobj          **xvec = NULL;
    size_t           xlen = 0;
    cxobj           *x;
    char            *bodystr;
    int              i;
    int              j;
    int              k;
    cg_var          *cv;

    if (argv == NULL || cvec_len(argv) != 2){
	clicon_err(OE_PLUGIN, 0, "%s: requires arguments: <db> <xmlkeyfmt>",
		   __FUNCTION__);
	goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: Error when accessing argument <db>");
	goto done;
    }
    dbstr  = cv_string_get(cv);
    if (strcmp(dbstr, "running") != 0 &&
	strcmp(dbstr, "candidate") != 0 &&
	strcmp(dbstr, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    if ((cv = cvec_i(argv, 1)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: Error when accessing argument <xkfmt>");
	goto done;
    }
    xkfmt = cv_string_get(cv);
    /* xkfmt = /interface/%s/address/%s
       --> ^/interface/eth0/address/.*$
       --> /interface/[name=eth0]/address
    */
    if (xmlkeyfmt2xpath(xkfmt, cvv, &xkpath) < 0)
	goto done;   
    if (clicon_rpc_get_config(h, dbstr, "/", &xt) < 0)
    	goto done;
    /* One round to detect duplicates 
     * XXX The code below would benefit from some cleanup
     */
    j = 0;
    if (xpath_vec(xt, xkpath, &xvec, &xlen) < 0) 
	goto done;
    for (i = 0; i < xlen; i++) {
	char *str;
	x = xvec[i];
	if (xml_type(x) == CX_BODY)
	    bodystr = xml_value(x);
	else
	    bodystr = xml_body(x);
	if (bodystr == NULL){
	    clicon_err(OE_CFG, 0, "No xml body");
	    goto done;
	}
	/* detect duplicates */
	for (k=0; k<j; k++){
	    if (xml_type(xvec[k]) == CX_BODY)
		str = xml_value(xvec[k]);
	    else
		str = xml_body(xvec[k]);
	    if (strcmp(str, bodystr)==0)
		break;
	}
	if (k==j) /* not duplicate */
	    xvec[j++] = x;
    }
    xlen = j;
    for (i = 0; i < xlen; i++) {
	x = xvec[i];
	if (xml_type(x) == CX_BODY)
	    bodystr = xml_value(x);
	else
	    bodystr = xml_body(x);
	if (bodystr == NULL){
	    clicon_err(OE_CFG, 0, "No xml body");
	    goto done;
	}
	/* XXX RFC3986 decode */
	cvec_add_string(commands, NULL, bodystr);
    }
    retval = 0;
  done:
    if (xvec)
	free(xvec);
    if (xt)
	xml_free(xt);
    if (xkpath) 
	free(xkpath);
    return retval;
}



/*! List files in a directory
 */
int
expand_dir(char *dir, int *nr, char ***commands, mode_t flags, int detail)
{
    DIR	*dirp;
    struct dirent *dp;
    struct stat st;
    char *str;
    char *cmd;
    int len;
    int retval = -1;
    struct passwd *pw;
    char filename[MAXPATHLEN];

    if ((dirp = opendir(dir)) == 0){
	fprintf(stderr, "expand_dir: opendir(%s) %s\n", 
		dir, strerror(errno));
	return -1;
    }
    *nr = 0;
    while ((dp = readdir(dirp)) != NULL) {
	if (
#if 0
	    strcmp(dp->d_name, ".") != 0 &&
	    strcmp(dp->d_name, "..") != 0
#else
	    dp->d_name[0] != '.'
#endif	    
	    ) {
	    snprintf(filename, MAXPATHLEN-1, "%s/%s", dir, dp->d_name);
	    if (lstat(filename, &st) == 0){
		if ((st.st_mode & flags) == 0)
		    continue;

#if EXPAND_RECURSIVE
		if (S_ISDIR(st.st_mode)) {
		    int nrsav = *nr;
		    if(expand_dir(filename, nr, commands, detail) < 0)
			goto quit;
		    while(nrsav < *nr) {
			len = strlen(dp->d_name) +  strlen((*commands)[nrsav]) + 2;
			if((str = malloc(len)) == NULL) {
			    fprintf(stderr, "expand_dir: malloc: %s\n",
				    strerror(errno));
			    goto quit;
			}
			snprintf(str, len-1, "%s/%s",
				 dp->d_name, (*commands)[nrsav]);
			free((*commands)[nrsav]);
			(*commands)[nrsav] = str;
			
			nrsav++;
		    }
		    continue;
		}
#endif
		if ((cmd = strdup(dp->d_name)) == NULL) {
		    fprintf(stderr, "expand_dir: strdup: %s\n",
			    strerror(errno));
		    goto quit;
		}
		if (0 &&detail){
		    if ((pw = getpwuid(st.st_uid)) == NULL){
			fprintf(stderr, "expand_dir: getpwuid(%d): %s\n",
				st.st_uid, strerror(errno));
			goto quit;
		    }
		    len = strlen(cmd) + 
			strlen(pw->pw_name) +
#ifdef __FreeBSD__
			strlen(ctime(&st.st_mtimespec.tv_sec)) +
#else
			strlen(ctime(&st.st_mtim.tv_sec)) +
#endif

			strlen("{ by }") + 1 /* \0 */;
		    if ((str=realloc(cmd, strlen(cmd)+len)) == NULL) {
			fprintf(stderr, "expand_dir: malloc: %s\n",
				strerror(errno));
			goto quit;
		    }
		    snprintf(str + strlen(dp->d_name), 
			     len - strlen(dp->d_name),
			     "{%s by %s}",
#ifdef __FreeBSD__
			     ctime(&st.st_mtimespec.tv_sec),
#else
			     ctime(&st.st_mtim.tv_sec),
#endif

			     pw->pw_name
			);
		    cmd = str;
		}
		if (((*commands) =
		     realloc(*commands, ((*nr)+1)*sizeof(char**))) == NULL){
		    perror("expand_dir: realloc");
		    goto quit;
		}
		(*commands)[(*nr)] = cmd;
		(*nr)++;
		if (*nr >= 128) /* Limit number of options */
		    break;
	    }
	}
    }
    retval = 0;
  quit:
    closedir(dirp);
    return retval;
}



/*! CLI callback show yang spec. If arg given matches yang argument string */
int
show_yangv(clicon_handle h, 
	   cvec         *cvv, 
	   cvec         *argv)
{
  yang_node *yn;
  char      *str = NULL;
  yang_spec *yspec;

  yspec = clicon_dbspec_yang(h);	
  if (cvec_len(argv) > 0){
      str = cv_string_get(cvec_i(argv, 0));
      yn = (yang_node*)yang_find((yang_node*)yspec, 0, str);
  }
  else
    yn = (yang_node*)yspec;
  yang_print(stdout, yn, 0);
  return 0;
}


#ifdef notused
/*! XML to CSV raw variant 
 * @see xml2csv
 */
static int 
xml2csv_raw(FILE *f, cxobj *x)
{
    cxobj           *xc;
    cxobj           *xb;
    int              retval = -1;
    int              i = 0;

    xc = NULL;
    while ((xc = xml_child_each(x, xc, CX_ELMNT)) != NULL) {
	if (xml_child_nr(xc)){
	    xb = xml_child_i(xc, 0);
	    if (xml_type(xb) == CX_BODY){
		if (i++)
		    fprintf(f, ";");
		fprintf(f, "%s", xml_value(xb));
	    }
	}
    }
    fprintf(f, "\n");
    retval = 0;
    return retval;
}
#endif

/*! Translate XML -> CSV commands
 * Can only be made in a 'flat tree', ie on the form:
 * <X><A>B</A></X> --> 
 * Type, A
 * X,  B
 * @param[in]  f     Output file
 * @param[in]  x     XML tree
 * @param[in]  cvv   A vector of field names present in XML
 * This means that only fields in x that are listed in cvv will be printed.
 */
static int 
xml2csv(FILE *f, cxobj *x, cvec *cvv)
{
    cxobj *xe, *xb;
    int              retval = -1;
    cg_var          *vs;

    fprintf(f, "%s", xml_name(x));
    xe = NULL;

    vs = NULL;
    while ((vs = cvec_each(cvv, vs))) {
	if ((xe = xml_find(x, cv_name_get(vs))) == NULL){
	    fprintf(f, ";");
	    continue;
	}
	if (xml_child_nr(xe)){
	    xb = xml_child_i(xe, 0);
	    fprintf(f, ";%s", xml_value(xb));
	}
    }
    fprintf(f, "\n");
    retval = 0;
    return retval;
}

/*! Generic function for showing configurations.
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @param[out] xt    Configuration as xml tree.
 * Format of arg:
 *   <dbname>  "running", "candidate", "startup"
 *   <xpath>   xpath expression 
 *   <varname> optional name of variable in cvv. If set, xpath must have a '%s'
 * @code
 *   show config id <n:string>, show_conf_as("running interfaces/interface[name=%s] n");
 * @endcode
 */
static int
show_confv_as(clicon_handle h, 
	      cvec         *cvv, 
	      cvec        *argv, 
	      cxobj       **xt) /* top xml */
{
    int              retval = -1;
    char            *db;
    char            *xpath;
    char            *attr = NULL;
    cbuf            *cbx = NULL;
    int              i;
    int              j;
    cg_var          *cvattr;
    char            *val = NULL;
    
    if (cvec_len(argv) != 2 && cvec_len(argv) != 3){
	if (cvec_len(argv)==1)
	    clicon_err(OE_PLUGIN, 0, "Got single argument:\"%s\". Expected \"<dbname>,<xpath>[,<attr>]\"", cv_string_get(cvec_i(argv,0)));
	else
	    clicon_err(OE_PLUGIN, 0, "Got %d arguments. Expected: <dbname>,<xpath>[,<attr>]", cvec_len(argv));

	goto done;
    }
    /* Dont get attr here, take it from arg instead */
    db = cv_string_get(cvec_i(argv, 0));
    if (strcmp(db, "running") != 0 && 
	strcmp(db, "candidate") != 0 && 
	strcmp(db, "startup") != 0)	{
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", db);	
	goto done;
    }
    xpath = cv_string_get(cvec_i(argv, 1));
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_PLUGIN, errno, "cbuf_new");	
	goto done;
    }
    if (cvec_len(argv) == 3){
	attr = cv_string_get(cvec_i(argv, 2));
	j = 0;
	for (i=0; i<strlen(xpath); i++)
	    if (xpath[i] == '%')
		j++;
	if (j != 1){
	    clicon_err(OE_PLUGIN, 0, "xpath '%s' does not have a single '%%'");	
	    goto done;
	}
	if ((cvattr = cvec_find_var(cvv, attr)) == NULL){
	    clicon_err(OE_PLUGIN, 0, "attr '%s' not found in cligen var list", attr);	
	    goto done;
	}
	if ((val = cv2str_dup(cvattr)) == NULL){
	    clicon_err(OE_PLUGIN, errno, "cv2str_dup");	
	    goto done;
	}
	cprintf(cbx, xpath, val);	
    }
    else
	cprintf(cbx, "%s", xpath);	
    if (clicon_rpc_get_config(h, db, cbuf_get(cbx), xt) < 0)
	goto done;
    retval = 0;
done:
    if (val)
	free(val);
    if (cbx)
	cbuf_free(cbx);
    return retval;
}

/*! Show a configuration database on stdout using XML format
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @param[in]  netconf If set print as netconf edit-config, otherwise just xml
 * @see show_conf_as  the main function
 */
static int
show_confv_as_xml1(clicon_handle h, 
		  cvec         *cvv, 
		   cvec         *argv,
		  int           netconf)
{
    cxobj *xt = NULL;
    cxobj *xc;
    int    retval = -1;

    if (show_confv_as(h, cvv, argv, &xt) < 0)
	goto done;
    if (netconf) /* netconf prefix */
	fprintf(stdout, "<rpc><edit-config><target><candidate/></target><config>\n");
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	clicon_xml2file(stdout, xc, netconf?2:0, 1);
    if (netconf) /* netconf postfix */
	fprintf(stdout, "</config></edit-config></rpc>]]>]]>\n");
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    return retval;
}

/*! Show configuration as prettyprinted xml 
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @see show_conf_as  the main function
 */
int
show_confv_as_xml(clicon_handle h, 
		  cvec         *cvv, 
		  cvec         *argv)
{
    return show_confv_as_xml1(h, cvv, argv, 0);
}

/*! Show configuration as prettyprinted xml with netconf hdr/tail
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @see show_conf_as  the main function
 */
int
show_confv_as_netconf(clicon_handle h, 
		     cvec         *cvv, 
		     cvec         *argv)
{
    return show_confv_as_xml1(h, cvv, argv, 1);
}

/*! Show configuration as JSON
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @see show_conf_as  the main function
 */
int
show_confv_as_json(clicon_handle h, 
		  cvec         *cvv, 
		  cvec         *argv)
{
    cxobj *xt = NULL;
    int    retval = -1;

    if (show_confv_as(h, cvv, argv, &xt) < 0)
	goto done;
    xml2json(stdout, xt, 1);
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    return retval;
}


/*! Show configuration as text
 * Utility function used by cligen spec file
 */
static int
show_confv_as_text1(clicon_handle h, 
		    cvec         *cvv, 
		    cvec         *argv)
{
    cxobj       *xt = NULL;
    cxobj       *xc;
    int          retval = -1;

    if (show_confv_as(h, cvv, argv, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	xml2txt(stdout, xc, 0); /* tree-formed text */
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    return retval;
}


/* Show configuration as commands, ie not tree format but as one-line commands
 */
static int
show_confv_as_command(clicon_handle h, 
		      cvec         *cvv, 
		      cvec         *argv,
		      char         *prepend)
{
    cxobj             *xt = NULL;
    cxobj             *xc;
    enum genmodel_type gt;
    int                retval = -1;

    if ((xt = xml_new("tmp", NULL)) == NULL)
	goto done;
    if (show_confv_as(h, cvv, argv, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL){
	if ((gt = clicon_cli_genmodel_type(h)) == GT_ERR)
	    goto done;
	xml2cli(stdout, xc, prepend, gt, __FUNCTION__); /* cli syntax */
    }
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__);
    return retval;
}

int
show_confv_as_text(clicon_handle h, 
		   cvec         *cvv, 
		   cvec         *argv)
{
    return show_confv_as_text1(h, cvv, argv);
}

int
show_confv_as_cli(clicon_handle h, 
		  cvec         *cvv, 
		  cvec         *argv)
{
    return show_confv_as_command(h, cvv, argv, NULL); /* XXX: how to set prepend? */
}

static int
show_confv_as_csv1(clicon_handle h, 
		   cvec         *cvv0, 
		   cvec         *argv)
{
    cxobj      *xt = NULL;
    cxobj      *xc;
    int         retval = -1;
    cvec       *cvv=NULL;
    char       *str;

    if (show_confv_as(h, cvv0, argv, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL){
	if ((str = chunk_sprintf(__FUNCTION__, "%s[]", xml_name(xc))) == NULL)
	    goto done;
#ifdef NOTYET /* yang-spec? */
	if (ds==NULL && (ds = key2spec_key(dbspec, str)) != NULL){
	    cg_var     *vs;
	    fprintf(stdout, "Type");
	    cvv = db_spec2cvec(ds);
	    vs = NULL;
	    while ((vs = cvec_each(cvv, vs))) 
		fprintf(stdout, ";%s",	cv_name_get(vs));
	    fprintf(stdout, "\n");
	} /* Now values just need to follow,... */
#endif /* yang-spec? */
	if (cvv== NULL)
	    goto done;
	xml2csv(stdout, xc, cvv); /* csv syntax */
    }
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__);
    return retval;
}

int
show_confv_as_csv(clicon_handle h, 
		  cvec         *cvv, 
		  cvec         *argv)
{
    return show_confv_as_csv1(h, cvv, argv);
}

/*! Show configuration as text given an xpath
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath>
 * @note Hardcoded that a variable in cvv is named "xpath"
 */
int
show_confv_xpath(clicon_handle h, 
		 cvec         *cvv, 
		 cvec         *argv)
{
    int              retval = -1;
    char            *str;
    char            *xpath;
    cg_var          *cv;
    cxobj           *xt = NULL;
    cxobj          **xv = NULL;
    size_t           xlen;
    int              i;

    if (cvec_len(argv) != 1){
	clicon_err(OE_PLUGIN, 0, "%s: Requires one element to be <dbname>", __FUNCTION__);
	goto done;
    }
    str = cv_string_get(cvec_i(argv, 0));
    /* Dont get attr here, take it from arg instead */
    if (strcmp(str, "running") != 0 && 
	strcmp(str, "candidate") != 0 && 
	strcmp(str, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", str);	
	goto done;
    }
    cv = cvec_find_var(cvv, "xpath");
    xpath = cv_string_get(cv);
    if (clicon_rpc_get_config(h, str, xpath, &xt) < 0)
    	goto done;
    if (xpath_vec(xt, xpath, &xv, &xlen) < 0) 
	goto done;
    for (i=0; i<xlen; i++)
	xml_print(stdout, xv[i]);

    retval = 0;
done:
    if (xv)
	free(xv);
    if (xt)
	xml_free(xt);
    return retval;
}


/*=================================================================
 * Here are backward compatible cligen callback functions used when 
 * the option: CLICON_CLIGEN_CALLBACK_SINGLE_ARG is set.
 */

cb_single_arg(show_yang)

/*! This is obsolete version of expandv_dbvar
 * If CLICON_CLIGEN_EXPAND_SINGLE_ARG is set
*/
int
expand_dbvar(void   *h, 
	     char   *name, 
	     cvec   *cvv, 
	     cg_var *arg, 
	     int    *nr, 
	     char ***commands, 
	     char ***helptexts)
{
    int              nvec;
    char           **vec = NULL;
    int              retval = -1;
    char            *xkfmt;
    char            *str;
    char            *dbstr;    
    cxobj           *xt = NULL;
    char            *xkpath = NULL;
    cxobj          **xvec = NULL;
    size_t           xlen = 0;
    cxobj           *x;
    char            *bodystr;
    int              i;
    int              j;
    int              k;
    int              i0;

    if (arg == NULL || (str = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    /* In the example, str = "candidate /x/m1/%s/b" */
    if ((vec = clicon_strsplit(str, " ", &nvec, __FUNCTION__)) == NULL){
	clicon_err(OE_PLUGIN, errno, "clicon_strsplit");	
	goto done;
    }
    dbstr  = vec[0];
    if (strcmp(dbstr, "running") != 0 &&
	strcmp(dbstr, "candidate") != 0 &&
	strcmp(dbstr, "startup") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    xkfmt = vec[1];
    /* xkfmt = /interface=%s/address=%s
       --> /interface=eth0/address=1.2.3.4
    */
    if (xmlkeyfmt2xpath(xkfmt, cvv, &xkpath) < 0)
	goto done;   
    if (clicon_rpc_get_config(h, dbstr, "/", &xt) < 0)
    	goto done;
    if (xpath_vec(xt, xkpath, &xvec, &xlen) < 0) 
	goto done;
    /* One round to detect duplicates 
     * XXX The code below would benefit from some cleanup
     */
    j = 0;
    for (i = 0; i < xlen; i++) {
	char *str;
	x = xvec[i];
	if (xml_type(x) == CX_BODY)
	    bodystr = xml_value(x);
	else
	    bodystr = xml_body(x);
	if (bodystr == NULL){
	    clicon_err(OE_CFG, 0, "No xml body");
	    goto done;
	}
	/* detect duplicates */
	for (k=0; k<j; k++){
	    if (xml_type(xvec[k]) == CX_BODY)
		str = xml_value(xvec[k]);
	    else
		str = xml_body(xvec[k]);
	    if (strcmp(str, bodystr)==0)
		break;
	}
	if (k==j) /* not duplicate */
	    xvec[j++] = x;
    }
    xlen = j;
    i0 = *nr;
    *nr += xlen;
    if ((*commands = realloc(*commands, sizeof(char *) * (*nr))) == NULL) {
	clicon_err(OE_UNIX, errno, "realloc: %s", strerror (errno));	
	goto done;
    }
    for (i = 0; i < xlen; i++) {
	x = xvec[i];
	if (xml_type(x) == CX_BODY)
	    bodystr = xml_value(x);
	else
	    bodystr = xml_body(x);
	if (bodystr == NULL){
	    clicon_err(OE_CFG, 0, "No xml body");
	    goto done;
	}
	(*commands)[i0+i] = strdup(bodystr);
    }
    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    if (xvec)
	free(xvec);
    if (xt)
	xml_free(xt);
    if (xkpath) 
	free(xkpath);
    return retval;
}



/*! Generic function for showing configurations.
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @param[out] xt    Configuration as xml tree.
 * Format of arg:
 *   <dbname>  "running", "candidate", "startup"
 *   <xpath>   xpath expression 
 *   <varname> optional name of variable in cvv. If set, xpath must have a '%s'
 * @code
 *   show config id <n:string>, show_conf_as("running interfaces/interface[name=%s] n");
 * @endcode
 */
static int
show_conf_as(clicon_handle h, 
	     cvec         *cvv, 
	     cg_var       *arg, 
	     cxobj       **xt) /* top xml */
{
    int              retval = -1;
    char            *db;
    char           **vec = NULL;
    int              nvec;
    char            *str;
    char            *xpath;
    char            *attr = NULL;
    cbuf            *cbx = NULL;
    int              i;
    int              j;
    cg_var          *cvattr;
    char            *val = NULL;
    
    if (arg == NULL || (str = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    if ((vec = clicon_strsplit(str, " ", &nvec, __FUNCTION__)) == NULL){
	clicon_err(OE_PLUGIN, errno, "clicon_strsplit");	
	goto done;
    }
    if (nvec != 2 && nvec != 3){
	clicon_err(OE_PLUGIN, 0, "format error \"%s\" - expected <dbname> <xpath> [<attr>] got %d arg", str, nvec);	
	goto done;
    }
    /* Dont get attr here, take it from arg instead */
    db = vec[0];
    if (strcmp(db, "running") != 0 && 
	strcmp(db, "candidate") != 0 && 
	strcmp(db, "startup") != 0) {
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", db);	
	goto done;
    }
    xpath = vec[1];
    if ((cbx = cbuf_new()) == NULL){
	clicon_err(OE_PLUGIN, errno, "cbuf_new");	
	goto done;
    }
    if (nvec == 3){
	attr = vec[2];
	j = 0;
	for (i=0; i<strlen(xpath); i++)
	    if (xpath[i] == '%')
		j++;
	if (j != 1){
	    clicon_err(OE_PLUGIN, 0, "xpath '%s' does not have a single '%%'");	
	    goto done;
	}
	if ((cvattr = cvec_find_var(cvv, attr)) == NULL){
	    clicon_err(OE_PLUGIN, 0, "attr '%s' not found in cligen var list", attr);	
	    goto done;
	}
	if ((val = cv2str_dup(cvattr)) == NULL){
	    clicon_err(OE_PLUGIN, errno, "cv2str_dup");	
	    goto done;
	}
	cprintf(cbx, xpath, val);	
    }
    else
	cprintf(cbx, "%s", xpath);	
    if (clicon_rpc_get_config(h, db, cbuf_get(cbx), xt) < 0)
	goto done;
    retval = 0;
done:
    if (val)
	free(val);
    if (cbx)
	cbuf_free(cbx);
    unchunk_group(__FUNCTION__);
    return retval;
}


/*! Show a configuration database on stdout using XML format
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @param[in]  netconf If set print as netconf edit-config, otherwise just xml
 * @see show_conf_as  the main function
 */
static int
show_conf_as_xml1(clicon_handle h, 
		  cvec         *cvv, 
		  cg_var       *arg, 
		  int           netconf)
{
    cxobj *xt = NULL;
    cxobj *xc;
    int    retval = -1;

    if (show_conf_as(h, cvv, arg, &xt) < 0)
	goto done;
    if (netconf) /* netconf prefix */
	fprintf(stdout, "<rpc><edit-config><target><candidate/></target><config>\n");
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	clicon_xml2file(stdout, xc, netconf?2:0, 1);
    if (netconf) /* netconf postfix */
	fprintf(stdout, "</config></edit-config></rpc>]]>]]>\n");
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    return retval;

}

/*! Show configuration as prettyprinted xml 
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @see show_conf_as  the main function
 */
int
show_conf_as_xml(clicon_handle h, 
		 cvec *cvv, 
		 cg_var *arg)
{
    return show_conf_as_xml1(h, cvv, arg, 0);
}

/*! Show configuration as prettyprinted xml with netconf hdr/tail
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @see show_conf_as  the main function
 */
int
show_conf_as_netconf(clicon_handle h, 
		     cvec         *cvv, 
		     cg_var       *arg)
{
    return show_conf_as_xml1(h, cvv, arg, 1);
}

/*! Show configuration as JSON
 * Utility function used by cligen spec file
 * @param[in]  h     CLICON handle
 * @param[in]  cvv   Vector of variables from CLIgen command-line
 * @param[in]  arg   A string: <dbname> <xpath> [<varname>]
 * @see show_conf_as  the main function
 */
int
show_conf_as_json(clicon_handle h, 
		  cvec         *cvv, 
		  cg_var       *arg)
{
    cxobj *xt = NULL;
    int    retval = -1;

    if (show_conf_as(h, cvv, arg, &xt) < 0)
	goto done;
    xml2json(stdout, xt, 1);
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    return retval;
}


/*! Show configuration as text
 * Utility function used by cligen spec file
 */
static int
show_conf_as_text1(clicon_handle h, cvec *cvv, cg_var *arg)
{
    cxobj       *xt = NULL;
    cxobj       *xc;
    int          retval = -1;

    if (show_conf_as(h, cvv, arg, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	xml2txt(stdout, xc, 0); /* tree-formed text */
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__);
    return retval;
}


/* Show configuration as commands, ie not tree format but as one-line commands
 */
static int
show_conf_as_command(clicon_handle h, cvec *cvv, cg_var *arg, char *prepend)
{
    cxobj             *xt = NULL;
    cxobj             *xc;
    enum genmodel_type gt;
    int                retval = -1;

    if ((xt = xml_new("tmp", NULL)) == NULL)
	goto done;
    if (show_conf_as(h, cvv, arg, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL){
	if ((gt = clicon_cli_genmodel_type(h)) == GT_ERR)
	    goto done;
	xml2cli(stdout, xc, prepend, gt, __FUNCTION__); /* cli syntax */
    }
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__);
    return retval;
}

int
show_conf_as_text(clicon_handle h, cvec *cvv, cg_var *arg)
{
    return show_conf_as_text1(h, cvv, arg);
}

int
show_conf_as_cli(clicon_handle h, cvec *cvv, cg_var *arg)
{
    return show_conf_as_command(h, cvv, arg, NULL); /* XXX: how to set prepend? */
}

static int
show_conf_as_csv1(clicon_handle h, cvec *cvv0, cg_var *arg)
{
    cxobj      *xt = NULL;
    cxobj      *xc;
    int         retval = -1;
    cvec       *cvv=NULL;
    char       *str;

    if (show_conf_as(h, cvv0, arg, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL){
	if ((str = chunk_sprintf(__FUNCTION__, "%s[]", xml_name(xc))) == NULL)
	    goto done;
#ifdef NOTYET /* yang-spec? */
	if (ds==NULL && (ds = key2spec_key(dbspec, str)) != NULL){
	    cg_var     *vs;
	    fprintf(stdout, "Type");
	    cvv = db_spec2cvec(ds);
	    vs = NULL;
	    while ((vs = cvec_each(cvv, vs))) 
		fprintf(stdout, ";%s",	cv_name_get(vs));
	    fprintf(stdout, "\n");
	} /* Now values just need to follow,... */
#endif /* yang-spec? */
	if (cvv== NULL)
	    goto done;
	xml2csv(stdout, xc, cvv); /* csv syntax */
    }
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__);
    return retval;
}

int
show_conf_as_csv(clicon_handle h, cvec *cvv, cg_var *arg)
{
    return show_conf_as_csv1(h, cvv, arg);
}

