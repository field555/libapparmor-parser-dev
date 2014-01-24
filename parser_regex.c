/*
 *   Copyright (c) 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007
 *   NOVELL (All rights reserved)
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, contact Novell, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <libintl.h>
#include <linux/limits.h>
#include <sys/apparmor.h>
#define _(s) gettext(s)

#include <string>

/* #define DEBUG */

#include "parser.h"
#include "profile.h"
#include "libapparmor_re/apparmor_re.h"
#include "libapparmor_re/aare_rules.h"
#include "mount.h"
#include "dbus.h"
#include "policydb.h"

enum error_type {
	e_no_error,
	e_parse_error,
	e_buffer_overflow
};

/* Filters out multiple slashes (except if the first two are slashes,
 * that's a distinct namespace in linux) and trailing slashes.
 * NOTE: modifies in place the contents of the path argument */

static void filter_slashes(char *path)
{
	char *sptr, *dptr;
	BOOL seen_slash = 0;

	if (!path || (strlen(path) < 2))
		return;

	sptr = dptr = path;

	/* special case for linux // namespace */
	if (sptr[0] == '/' && sptr[1] == '/' && sptr[2] != '/') {
		sptr += 2;
		dptr += 2;
	}

	while (*sptr) {
		if (*sptr == '/') {
			if (seen_slash) {
				++sptr;
			} else {
				*dptr++ = *sptr++;
				seen_slash = TRUE;
			}
		} else {
			seen_slash = 0;
			if (dptr < sptr) {
				*dptr++ = *sptr++;
			} else {
				dptr++;
				sptr++;
			}
		}
	}
	*dptr = 0;
}

/* converts the apparmor regex in aare and appends pcre regex output
 * to pcre string */
static pattern_t convert_aaregex_to_pcre(const char *aare, int anchor,
					 std::string& pcre, int *first_re_pos)
{
#define update_re_pos(X) if (!(*first_re_pos)) { *first_re_pos = (X); }
#define MAX_ALT_DEPTH 50
	*first_re_pos = 0;

	int ret = TRUE;
	/* flag to indicate input error */
	enum error_type error;

	const char *sptr;
	pattern_t ptype;

	BOOL bEscape = 0;	/* flag to indicate escape */
	int ingrouping = 0;	/* flag to indicate {} context */
	int incharclass = 0;	/* flag to indicate [ ] context */
	int grouping_count[MAX_ALT_DEPTH];

	error = e_no_error;
	ptype = ePatternBasic;	/* assume no regex */

	sptr = aare;

	if (dfaflags & DFA_DUMP_RULE_EXPR)
		fprintf(stderr, "aare: %s   ->   ", aare);

	if (anchor)
		/* anchor beginning of regular expression */
		pcre.append("^");

	while (error == e_no_error && *sptr) {
		switch (*sptr) {

		case '\\':
			/* concurrent escapes are allowed now and
			 * output as two consecutive escapes so that
			 * pcre won't interpret them
			 * \\\{...\\\} will be emitted as \\\{...\\\}
			 * for pcre matching.  For string matching
			 * and globbing only one escape is output
			 * this is done by stripping later
			 */
			if (bEscape) {
				pcre.append("\\\\");
			} else {
				bEscape = TRUE;
				++sptr;
				continue;	/*skip turning bEscape off */
			}	/* bEscape */
			break;
		case '*':
			if (bEscape) {
				/* '*' is a PCRE special character */
				/* We store an escaped *, in case we
				 * end up using this regex buffer (i.e another
				 * non-escaped regex follows)
				 */
				pcre.append("\\*");
			} else {
				if ((pcre.length() > 0) && pcre[pcre.length() - 1]  == '/') {
					#if 0
					// handle comment containing use
					// of C comment characters
					// /* /*/ and /** to describe paths
					//
					// modify what is emitted for * and **
					// when used as the only path
					// component
					// ex.
					// /* /*/ /**/ /**
					// this prevents these expressions
					// from matching directories or
					// invalid paths
					// in these case * and ** must
					// match at least 1 character to
					// get a valid path element.
					// ex.
					// /foo/* -> should not match /foo/
					// /foo/*bar -> should match /foo/bar
					// /*/foo -> should not match //foo
					#endif
					const char *s = sptr;
					while (*s == '*')
						s++;
					if (*s == '/' || !*s) {
						pcre.append("[^/\\x00]");
					}
				}
				if (*(sptr + 1) == '*') {
					/* is this the first regex form we
					 * have seen and also the end of
					 * pattern? If so, we can use
					 * optimised tail globbing rather
					 * than full regex.
					 */
					update_re_pos(sptr - aare);
					if (*(sptr + 2) == '\0' &&
					    ptype == ePatternBasic) {
						ptype = ePatternTailGlob;
					} else {
						ptype = ePatternRegex;
					}

					pcre.append("[^\\x00]*");
					sptr++;
				} else {
					update_re_pos(sptr - aare);
					ptype = ePatternRegex;
					pcre.append("[^/\\x00]*");
				}	/* *(sptr+1) == '*' */
			}	/* bEscape */

			break;

		case '?':
			if (bEscape) {
				/* ? is not a PCRE regex character
				 * so no need to escape, just skip
				 * transform
				 */
				pcre.append(1, *sptr);
			} else {
				update_re_pos(sptr - aare);
				ptype = ePatternRegex;
				pcre.append("[^/\\x00]");
			}
			break;

		case '[':
			if (bEscape) {
				/* [ is a PCRE special character */
				pcre.append("\\[");
			} else {
				update_re_pos(sptr - aare);
				incharclass = 1;
				ptype = ePatternRegex;
				pcre.append(1, *sptr);
			}
			break;

		case ']':
			if (bEscape) {
				/* ] is a PCRE special character */
				pcre.append("\\]");
			} else {
				if (incharclass == 0) {
					error = e_parse_error;
					PERROR(_("%s: Regex grouping error: Invalid close ], no matching open [ detected\n"), progname);
				}
				incharclass = 0;
				pcre.append(1, *sptr);
			}
			break;

		case '{':
			if (bEscape) {
				/* { is a PCRE special character */
				pcre.append("\\{");
			} else {
				if (incharclass) {
					/* don't expand inside [] */
					pcre.append("{");
				} else {
					update_re_pos(sptr - aare);
					ingrouping++;
					if (ingrouping >= MAX_ALT_DEPTH) {
						error = e_parse_error;
						PERROR(_("%s: Regex grouping error: Exceeded maximum nesting of {}\n"), progname);

					} else {
						grouping_count[ingrouping] = 0;
						ptype = ePatternRegex;
						pcre.append("(");
					}
				}	/* incharclass */
			}
			break;

		case '}':
			if (bEscape) {
				/* { is a PCRE special character */
				pcre.append("\\}");
			} else {
				if (incharclass) {
					/* don't expand inside [] */
					pcre.append("}");
				} else {
					if (grouping_count[ingrouping] == 0) {
						error = e_parse_error;
						PERROR(_("%s: Regex grouping error: Invalid number of items between {}\n"), progname);

					}
					ingrouping--;
					if (ingrouping < 0) {
						error = e_parse_error;
						PERROR(_("%s: Regex grouping error: Invalid close }, no matching open { detected\n"), progname);
						ingrouping = 0;
					}
					pcre.append(")");
				}	/* incharclass */
			}	/* bEscape */

			break;

		case ',':
			if (bEscape) {
				if (incharclass) {
					/* escape inside char class is a
					 * valid matching char for '\'
					 */
					pcre.append("\\,");
				} else {
					/* ',' is not a PCRE regex character
					 * so no need to escape, just skip
					 * transform
					 */
					pcre.append(1, *sptr);
				}
			} else {
				if (ingrouping && !incharclass) {
					grouping_count[ingrouping]++;
					pcre.append("|");
				} else {
					pcre.append(1, *sptr);
				}
			}
			break;

			/* these are special outside of character
			 * classes but not in them */
		case '^':
		case '$':
			if (incharclass) {
				pcre.append(1, *sptr);
			} else {
				pcre.append("\\");
				pcre.append(1, *sptr);
			}
			break;

			/*
			 * Not a subdomain regex, but needs to be
			 * escaped as it is a pcre metacharacter which
			 * we don't want to support. We always escape
			 * these, so no need to check bEscape
			 */
		case '.':
		case '+':
		case '|':
		case '(':
		case ')':
			pcre.append("\\");
			// fall through to default

		default:
			if (bEscape) {
				/* quoting mark used for something that
				 * does not need to be quoted; give a warning */
				pwarn("Character %c was quoted unnecessarily, "
				      "dropped preceding quote ('\\') character\n", *sptr);
			}
			pcre.append(1, *sptr);
			break;
		}	/* switch (*sptr) */

		bEscape = FALSE;
		++sptr;
	}		/* while error == e_no_error && *sptr) */

	if (ingrouping > 0 || incharclass) {
		error = e_parse_error;

		PERROR(_("%s: Regex grouping error: Unclosed grouping or character class, expecting close }\n"),
		       progname);
	}

	if ((error == e_no_error) && bEscape) {
		/* trailing backslash quote */
		error = e_parse_error;
		PERROR(_("%s: Regex error: trailing '\\' escape character\n"),
		       progname);
	}
	/* anchor end and terminate pattern string */
	if ((error == e_no_error) && anchor) {
		pcre.append("$");
	}
	/* check error  again, as above STORE may have set it */
	if (error != e_no_error) {
		if (error == e_buffer_overflow) {
			PERROR(_("%s: Internal buffer overflow detected, %d characters exceeded\n"),
			       progname, PATH_MAX);
		}

		PERROR(_("%s: Unable to parse input line '%s'\n"),
		       progname, aare);

		ret = FALSE;
		goto out;
	}

out:
	if (ret == FALSE)
		ptype = ePatternInvalid;

	if (dfaflags & DFA_DUMP_RULE_EXPR)
		fprintf(stderr, "%s\n", pcre.c_str());

	return ptype;
}

static const char *local_name(const char *name)
{
	const char *t;

	for (t = strstr(name, "//") ; t ; t = strstr(name, "//"))
		name = t + 2;

	return name;
}

static int process_profile_name_xmatch(Profile *prof)
{
	std::string tbuf;
	pattern_t ptype;
	const char *name;

	/* don't filter_slashes for profile names */
	if (prof->attachment)
		name = prof->attachment;
	else
		name = local_name(prof->name);
	ptype = convert_aaregex_to_pcre(name, 0, tbuf,
					&prof->xmatch_len);
	if (ptype == ePatternBasic)
		prof->xmatch_len = strlen(name);

	if (ptype == ePatternInvalid) {
		PERROR(_("%s: Invalid profile name '%s' - bad regular expression\n"), progname, name);
		return FALSE;
	} else if (ptype == ePatternBasic && !(prof->altnames || prof->attachment)) {
		/* no regex so do not set xmatch */
		prof->xmatch = NULL;
		prof->xmatch_len = 0;
		prof->xmatch_size = 0;
	} else {
		/* build a dfa */
		aare_ruleset_t *rule = aare_new_ruleset(0);
		if (!rule)
			return FALSE;
		if (!aare_add_rule(rule, tbuf.c_str(), 0, AA_MAY_EXEC, 0, dfaflags)) {
			aare_delete_ruleset(rule);
			return FALSE;
		}
		if (prof->altnames) {
			struct alt_name *alt;
			list_for_each(prof->altnames, alt) {
				int len;
				tbuf.clear();
				ptype = convert_aaregex_to_pcre(alt->name, 0,
								tbuf,
								&len);
				if (ptype == ePatternBasic)
					len = strlen(alt->name);
				if (len < prof->xmatch_len)
					prof->xmatch_len = len;
				if (!aare_add_rule(rule, tbuf.c_str(), 0, AA_MAY_EXEC, 0, dfaflags)) {
					aare_delete_ruleset(rule);
					return FALSE;
				}
			}
		}
		prof->xmatch = aare_create_dfa(rule, &prof->xmatch_size,
					      dfaflags);
		aare_delete_ruleset(rule);
		if (!prof->xmatch)
			return FALSE;
	}

	return TRUE;
}

static int process_dfa_entry(aare_ruleset_t *dfarules, struct cod_entry *entry)
{
	std::string tbuf;
	pattern_t ptype;
	int pos;

	if (!entry) 		/* shouldn't happen */
		return TRUE;


	if (entry->mode & ~AA_CHANGE_PROFILE)
		filter_slashes(entry->name);
	ptype = convert_aaregex_to_pcre(entry->name, 0, tbuf, &pos);
	if (ptype == ePatternInvalid)
		return FALSE;

	entry->pattern_type = ptype;

	/* ix implies m but the apparmor module does not add m bit to
	 * dfa states like it does for pcre
	 */
	if ((entry->mode >> AA_OTHER_SHIFT) & AA_EXEC_INHERIT)
		entry->mode |= AA_EXEC_MMAP << AA_OTHER_SHIFT;
	if ((entry->mode >> AA_USER_SHIFT) & AA_EXEC_INHERIT)
		entry->mode |= AA_EXEC_MMAP << AA_USER_SHIFT;

	/* relying on ptrace and change_profile not getting merged earlier */

	/* the link bit on the first pair entry should not get masked
	 * out by a deny rule, as both pieces of the link pair must
	 * match.  audit info for the link is carried on the second
	 * entry of the pair
	 */
	if (entry->deny && (entry->mode & AA_LINK_BITS)) {
		if (!aare_add_rule(dfarules, tbuf.c_str(), entry->deny,
				   entry->mode & ~AA_LINK_BITS,
				   entry->audit & ~AA_LINK_BITS, dfaflags))
			return FALSE;
	} else if (entry->mode & ~AA_CHANGE_PROFILE) {
		if (!aare_add_rule(dfarules, tbuf.c_str(), entry->deny, entry->mode,
				   entry->audit, dfaflags))
			return FALSE;
	}

	if (entry->mode & (AA_LINK_BITS)) {
		/* add the pair rule */
		std::string lbuf;
		int perms = AA_LINK_BITS & entry->mode;
		const char *vec[2];
		int pos;
		vec[0] = tbuf.c_str();
		if (entry->link_name) {
			ptype = convert_aaregex_to_pcre(entry->link_name, 0, lbuf, &pos);
			if (ptype == ePatternInvalid)
				return FALSE;
			if (entry->subset)
				perms |= LINK_TO_LINK_SUBSET(perms);
			vec[1] = lbuf.c_str();
		} else {
			perms |= LINK_TO_LINK_SUBSET(perms);
			vec[1] = "/[^/].*";
		}
		if (!aare_add_rule_vec(dfarules, entry->deny, perms, entry->audit & AA_LINK_BITS, 2, vec, dfaflags))
			return FALSE;
	}
	if (entry->mode & AA_CHANGE_PROFILE) {
		const char *vec[3];
		std::string lbuf;
		int index = 1;

		/* allow change_profile for all execs */
		vec[0] = "/[^\\x00]*";

		if (entry->ns) {
			int pos;
			ptype = convert_aaregex_to_pcre(entry->ns, 0, lbuf, &pos);
			vec[index++] = lbuf.c_str();
		}
		vec[index++] = tbuf.c_str();

		/* regular change_profile rule */
		if (!aare_add_rule_vec(dfarules, 0, AA_CHANGE_PROFILE | AA_ONEXEC, 0, index - 1, &vec[1], dfaflags))
			return FALSE;
		/* onexec rules - both rules are needed for onexec */
		if (!aare_add_rule_vec(dfarules, 0, AA_ONEXEC, 0, 1, vec, dfaflags))
			return FALSE;
		if (!aare_add_rule_vec(dfarules, 0, AA_ONEXEC, 0, index, vec, dfaflags))
			return FALSE;
	}
	if (entry->mode & (AA_USER_PTRACE | AA_OTHER_PTRACE)) {
		int mode = entry->mode & (AA_USER_PTRACE | AA_OTHER_PTRACE);
		if (entry->ns) {
			const char *vec[2];
			vec[0] = entry->ns;
			vec[1] = entry->name;
			if (!aare_add_rule_vec(dfarules, 0, mode, 0, 2, vec, dfaflags))
			    return FALSE;
		} else {
		  if (!aare_add_rule(dfarules, entry->name, 0, mode, 0, dfaflags))
				return FALSE;
		}
	}
	return TRUE;
}

int post_process_entries(Profile *prof)
{
	int ret = TRUE;
	struct cod_entry *entry;
	int count = 0;

	list_for_each(prof->entries, entry) {
		if (!process_dfa_entry(prof->dfa.rules, entry))
			ret = FALSE;
		count++;
	}

	prof->dfa.count = count;
	return ret;
}

int process_profile_regex(Profile *prof)
{
	int error = -1;

	if (!process_profile_name_xmatch(prof))
		goto out;

	prof->dfa.rules = aare_new_ruleset(0);
	if (!prof->dfa.rules)
		goto out;

	if (!post_process_entries(prof))
		goto out;

	if (prof->dfa.count > 0) {
		prof->dfa.dfa = aare_create_dfa(prof->dfa.rules, &prof->dfa.size,
						dfaflags);
		aare_delete_ruleset(prof->dfa.rules);
		prof->dfa.rules = NULL;
		if (!prof->dfa.dfa)
			goto out;
/*
		if (prof->dfa_size == 0) {
			PERROR(_("profile %s: has merged rules (%s) with "
				 "multiple x modifiers\n"),
			       prof->name, (char *) prof->dfa);
			free(prof->dfa);
			prof->dfa = NULL;
			goto out;
		}
*/
	}

	error = 0;

out:
	return error;
}

static int build_list_val_expr(std::string& buffer, struct value_list *list)
{
	struct value_list *ent;
	pattern_t ptype;
	int pos;

	if (!list) {
		buffer.append("[^\\000]*");
		return TRUE;
	}

	buffer.append("(");

	ptype = convert_aaregex_to_pcre(list->value, 0, buffer, &pos);
	if (ptype == ePatternInvalid)
		goto fail;

	list_for_each(list->next, ent) {
		buffer.append("|");
		ptype = convert_aaregex_to_pcre(ent->value, 0, buffer, &pos);
		if (ptype == ePatternInvalid)
			goto fail;
	}
	buffer.append(")");

	return TRUE;
fail:
	return FALSE;
}

static int convert_entry(std::string& buffer, char *entry)
{
	pattern_t ptype;
	int pos;

	if (entry) {
		ptype = convert_aaregex_to_pcre(entry, 0, buffer, &pos);
		if (ptype == ePatternInvalid)
			return FALSE;
	} else {
		buffer.append("[^\\000]*");
	}

	return TRUE;
}

static int build_mnt_flags(char *buffer, int size, unsigned int flags,
			   unsigned int inv_flags)
{
	char *p = buffer;
	int i, len = 0;

	if (flags == MS_ALL_FLAGS) {
		/* all flags are optional */
		len = snprintf(p, size, "[^\\000]*");
		if (len < 0 || len >= size)
			return FALSE;
		return TRUE;
	}
	for (i = 0; i <= 31; ++i) {
		if ((flags & inv_flags) & (1 << i))
			len = snprintf(p, size, "(\\x%02x|)", i + 1);
		else if (flags & (1 << i))
			len = snprintf(p, size, "\\x%02x", i + 1);
		else	/* no entry = not set */
			continue;

		if (len < 0 || len >= size)
			return FALSE;
		p += len;
		size -= len;
	}

	/* this needs to go once the backend is updated. */
	if (buffer == p) {
		/* match nothing - use impossible 254 as regex parser doesn't
		 * like the empty string
		 */
		if (size < 9)
			return FALSE;

		strcpy(p, "(\\xfe|)");
	}

	return TRUE;
}

static int build_mnt_opts(std::string& buffer, struct value_list *opts)
{
	struct value_list *ent;
	pattern_t ptype;
	int pos;

	if (!opts) {
		buffer.append("[^\\000]*");
		return TRUE;
	}

	list_for_each(opts, ent) {
		ptype = convert_aaregex_to_pcre(ent->value, 0, buffer, &pos);
		if (ptype == ePatternInvalid)
			return FALSE;

		if (ent->next)
			buffer.append(",");
	}

	return TRUE;
}

static int process_mnt_entry(aare_ruleset_t *dfarules, struct mnt_entry *entry)
{
	std::string mntbuf;
	std::string devbuf;
	std::string typebuf;
	char flagsbuf[PATH_MAX + 3];
	std::string optsbuf;
	char class_mount_hdr[64];
	const char *vec[5];
	int count = 0;
	unsigned int flags, inv_flags;

	sprintf(class_mount_hdr, "\\x%02x", AA_CLASS_MOUNT);

	/* a single mount rule may result in multiple matching rules being
	 * created in the backend to cover all the possible choices
	 */

	if ((entry->allow & AA_MAY_MOUNT) && (entry->flags & MS_REMOUNT)
	    && !entry->device && !entry->dev_type) {
		int allow;
		/* remount can't be conditional on device and type */
		/* rule class single byte header */
		mntbuf.assign(class_mount_hdr);
		if (entry->mnt_point) {
			/* both device && mnt_point or just mnt_point */
			if (!convert_entry(mntbuf, entry->mnt_point))
				goto fail;
			vec[0] = mntbuf.c_str();
		} else {
			if (!convert_entry(mntbuf, entry->device))
				goto fail;
			vec[0] = mntbuf.c_str();
		}
		/* skip device */
		devbuf.clear();
		if (!convert_entry(devbuf, NULL))
			goto fail;
		vec[1] = devbuf.c_str();
		/* skip type */
		vec[2] = devbuf.c_str();

		flags = entry->flags;
		inv_flags = entry->inv_flags;
		if (flags != MS_ALL_FLAGS)
			flags &= MS_REMOUNT_FLAGS;
		if (inv_flags != MS_ALL_FLAGS)
			flags &= MS_REMOUNT_FLAGS;
		if (!build_mnt_flags(flagsbuf, PATH_MAX, flags, inv_flags))
			goto fail;
		vec[3] = flagsbuf;

		if (entry->opts)
			allow = AA_MATCH_CONT;
		else
			allow = entry->allow;

		/* rule for match without required data || data MATCH_CONT */
		if (!aare_add_rule_vec(dfarules, entry->deny, allow,
				       entry->audit | AA_AUDIT_MNT_DATA, 4,
				       vec, dfaflags))
			goto fail;
		count++;

		if (entry->opts) {
			/* rule with data match required */
			optsbuf.clear();
			if (!build_mnt_opts(optsbuf, entry->opts))
				goto fail;
			vec[4] = optsbuf.c_str();
			if (!aare_add_rule_vec(dfarules, entry->deny,
					       entry->allow,
					       entry->audit | AA_AUDIT_MNT_DATA,
					       5, vec, dfaflags))
				goto fail;
			count++;
		}
	}
	if ((entry->allow & AA_MAY_MOUNT) && (entry->flags & MS_BIND)
	    && !entry->dev_type && !entry->opts) {
		/* bind mount rules can't be conditional on dev_type or data */
		/* rule class single byte header */
		mntbuf.assign(class_mount_hdr);
		if (!convert_entry(mntbuf, entry->mnt_point))
			goto fail;
		vec[0] = mntbuf.c_str();
		devbuf.clear();
		if (!convert_entry(devbuf, entry->device))
			goto fail;
		vec[1] = devbuf.c_str();
		typebuf.clear();
		if (!convert_entry(typebuf, NULL))
			goto fail;
		vec[2] = typebuf.c_str();

		flags = entry->flags;
		inv_flags = entry->inv_flags;
		if (flags != MS_ALL_FLAGS)
			flags &= MS_BIND_FLAGS;
		if (inv_flags != MS_ALL_FLAGS)
			flags &= MS_BIND_FLAGS;
		if (!build_mnt_flags(flagsbuf, PATH_MAX, flags, inv_flags))
			goto fail;
		vec[3] = flagsbuf;
		if (!aare_add_rule_vec(dfarules, entry->deny, entry->allow,
				       entry->audit, 4, vec, dfaflags))
			goto fail;
		count++;
	}
	if ((entry->allow & AA_MAY_MOUNT) &&
	    (entry->flags & (MS_UNBINDABLE | MS_PRIVATE | MS_SLAVE | MS_SHARED))
	    && !entry->device && !entry->dev_type && !entry->opts) {
		/* change type base rules can not be conditional on device,
		 * device type or data
		 */
		/* rule class single byte header */
		mntbuf.assign(class_mount_hdr);
		if (!convert_entry(mntbuf, entry->mnt_point))
			goto fail;
		vec[0] = mntbuf.c_str();
		/* skip device and type */
		devbuf.clear();
		if (!convert_entry(devbuf, NULL))
			goto fail;
		vec[1] = devbuf.c_str();
		vec[2] = devbuf.c_str();

		flags = entry->flags;
		inv_flags = entry->inv_flags;
		if (flags != MS_ALL_FLAGS)
			flags &= MS_MAKE_FLAGS;
		if (inv_flags != MS_ALL_FLAGS)
			flags &= MS_MAKE_FLAGS;
		if (!build_mnt_flags(flagsbuf, PATH_MAX, flags, inv_flags))
			goto fail;
		vec[3] = flagsbuf;
		if (!aare_add_rule_vec(dfarules, entry->deny, entry->allow,
				       entry->audit, 4, vec, dfaflags))
			goto fail;
		count++;
	}
	if ((entry->allow & AA_MAY_MOUNT) && (entry->flags & MS_MOVE)
	    && !entry->dev_type && !entry->opts) {
		/* mount move rules can not be conditional on dev_type,
		 * or data
		 */
		/* rule class single byte header */
		mntbuf.assign(class_mount_hdr);
		if (!convert_entry(mntbuf, entry->mnt_point))
			goto fail;
		vec[0] = mntbuf.c_str();
		devbuf.clear();
		if (!convert_entry(devbuf, entry->device))
			goto fail;
		vec[1] = devbuf.c_str();
		/* skip type */
		typebuf.clear();
		if (!convert_entry(typebuf, NULL))
			goto fail;
		vec[2] = typebuf.c_str();

		flags = entry->flags;
		inv_flags = entry->inv_flags;
		if (flags != MS_ALL_FLAGS)
			flags &= MS_MOVE_FLAGS;
		if (inv_flags != MS_ALL_FLAGS)
			flags &= MS_MOVE_FLAGS;
		if (!build_mnt_flags(flagsbuf, PATH_MAX, flags, inv_flags))
			goto fail;
		vec[3] = flagsbuf;
		if (!aare_add_rule_vec(dfarules, entry->deny, entry->allow,
				       entry->audit, 4, vec, dfaflags))
			goto fail;
		count++;
	}
	if ((entry->allow & AA_MAY_MOUNT) &&
	    (entry->flags | entry->inv_flags) & ~MS_CMDS) {
		int allow;
		/* generic mount if flags are set that are not covered by
		 * above commands
		 */
		/* rule class single byte header */
		mntbuf.assign(class_mount_hdr);
		if (!convert_entry(mntbuf, entry->mnt_point))
			goto fail;
		vec[0] = mntbuf.c_str();
		devbuf.clear();
		if (!convert_entry(devbuf, entry->device))
			goto fail;
		vec[1] = devbuf.c_str();
		typebuf.clear();
		if (!build_list_val_expr(typebuf, entry->dev_type))
			goto fail;
		vec[2] = typebuf.c_str();

		flags = entry->flags;
		inv_flags = entry->inv_flags;
		if (flags != MS_ALL_FLAGS)
			flags &= ~MS_CMDS;
		if (inv_flags != MS_ALL_FLAGS)
			flags &= ~MS_CMDS;
		if (!build_mnt_flags(flagsbuf, PATH_MAX, flags, inv_flags))
			goto fail;
		vec[3] = flagsbuf;

		if (entry->opts)
			allow = AA_MATCH_CONT;
		else
			allow = entry->allow;

		/* rule for match without required data || data MATCH_CONT */
		if (!aare_add_rule_vec(dfarules, entry->deny, allow,
				       entry->audit | AA_AUDIT_MNT_DATA, 4,
				       vec, dfaflags))
			goto fail;
		count++;

		if (entry->opts) {
			/* rule with data match required */
			optsbuf.clear();
			if (!build_mnt_opts(optsbuf, entry->opts))
				goto fail;
			vec[4] = optsbuf.c_str();
			if (!aare_add_rule_vec(dfarules, entry->deny,
					       entry->allow,
					       entry->audit | AA_AUDIT_MNT_DATA,
					       5, vec, dfaflags))
				goto fail;
			count++;
		}
	}
	if (entry->allow & AA_MAY_UMOUNT) {
		/* rule class single byte header */
		mntbuf.assign(class_mount_hdr);
		if (!convert_entry(mntbuf, entry->mnt_point))
			goto fail;
		vec[0] = mntbuf.c_str();
		if (!aare_add_rule_vec(dfarules, entry->deny, entry->allow,
				       entry->audit, 1, vec, dfaflags))
			goto fail;
		count++;
	}
	if (entry->allow & AA_MAY_PIVOTROOT) {
		/* rule class single byte header */
		mntbuf.assign(class_mount_hdr);
		if (!convert_entry(mntbuf, entry->mnt_point))
			goto fail;
		vec[0] = mntbuf.c_str();
		devbuf.clear();
		if (!convert_entry(devbuf, entry->device))
			goto fail;
		vec[1] = devbuf.c_str();
		if (!aare_add_rule_vec(dfarules, entry->deny, entry->allow,
				       entry->audit, 2, vec, dfaflags))
			goto fail;
		count++;
	}

	if (!count)
		/* didn't actually encode anything */
		goto fail;

	return TRUE;

fail:
	PERROR("Enocoding of mount rule failed\n");
	return FALSE;
}


static int process_dbus_entry(aare_ruleset_t *dfarules, struct dbus_entry *entry)
{
	std::string busbuf;
	std::string namebuf;
	std::string peer_labelbuf;
	std::string pathbuf;
	std::string ifacebuf;
	std::string memberbuf;
	char buffer[128];
	const char *vec[6];

	pattern_t ptype;
	int pos;

	if (!entry) 		/* shouldn't happen */
		return TRUE;

	sprintf(buffer, "\\x%02x", AA_CLASS_DBUS);
	busbuf.append(buffer);

	if (entry->bus) {
		ptype = convert_aaregex_to_pcre(entry->bus, 0, busbuf, &pos);
		if (ptype == ePatternInvalid)
			goto fail;
	} else {
		/* match any char except \000 0 or more times */
		busbuf.append("[^\\000]*");
	}
	vec[0] = busbuf.c_str();

	if (entry->name) {
		ptype = convert_aaregex_to_pcre(entry->name, 0, namebuf, &pos);
		if (ptype == ePatternInvalid)
			goto fail;
		vec[1] = namebuf.c_str();
	} else {
		/* match any char except \000 0 or more times */
		vec[1] = "[^\\000]*";
	}

	if (entry->peer_label) {
		ptype = convert_aaregex_to_pcre(entry->peer_label, 0,
						peer_labelbuf, &pos);
		if (ptype == ePatternInvalid)
			goto fail;
		vec[2] = peer_labelbuf.c_str();
	} else {
		/* match any char except \000 0 or more times */
		vec[2] = "[^\\000]*";
	}

	if (entry->path) {
		ptype = convert_aaregex_to_pcre(entry->path, 0, pathbuf, &pos);
		if (ptype == ePatternInvalid)
			goto fail;
		vec[3] = pathbuf.c_str();
	} else {
		/* match any char except \000 0 or more times */
		vec[3] = "[^\\000]*";
	}

	if (entry->interface) {
		ptype = convert_aaregex_to_pcre(entry->interface, 0, ifacebuf, &pos);
		if (ptype == ePatternInvalid)
			goto fail;
		vec[4] = ifacebuf.c_str();
	} else {
		/* match any char except \000 0 or more times */
		vec[4] = "[^\\000]*";
	}

	if (entry->member) {
		ptype = convert_aaregex_to_pcre(entry->member, 0, memberbuf, &pos);
		if (ptype == ePatternInvalid)
			goto fail;
		vec[5] = memberbuf.c_str();
	} else {
		/* match any char except \000 0 or more times */
		vec[5] = "[^\\000]*";
	}

	if (entry->mode & AA_DBUS_BIND) {
		if (!aare_add_rule_vec(dfarules, entry->deny,
				       entry->mode & AA_DBUS_BIND,
				       entry->audit & AA_DBUS_BIND,
				       2, vec, dfaflags))
			goto fail;
	}
	if (entry->mode & (AA_DBUS_SEND | AA_DBUS_RECEIVE)) {
		if (!aare_add_rule_vec(dfarules, entry->deny,
				entry->mode & (AA_DBUS_SEND | AA_DBUS_RECEIVE),
				entry->audit & (AA_DBUS_SEND | AA_DBUS_RECEIVE),
				6, vec, dfaflags))
			goto fail;
	}
	if (entry->mode & AA_DBUS_EAVESDROP) {
		if (!aare_add_rule_vec(dfarules, entry->deny,
				entry->mode & AA_DBUS_EAVESDROP,
				entry->audit & AA_DBUS_EAVESDROP,
				1, vec, dfaflags))
			goto fail;
	}
	return TRUE;

fail:
	return FALSE;
}

static int post_process_mnt_ents(Profile *prof)
{
	int ret = TRUE;
	int count = 0;

	/* Add fns for rules that should be added to policydb here */
	if (prof->mnt_ents && kernel_supports_mount) {
		struct mnt_entry *entry;
		list_for_each(prof->mnt_ents, entry) {
			if (!process_mnt_entry(prof->policy.rules, entry))
				ret = FALSE;
			count++;
		}
	} else if (prof->mnt_ents && !kernel_supports_mount)
		pwarn("profile %s mount rules not enforced\n", prof->name);

	prof->policy.count += count;

	return ret;
}

static int post_process_dbus_ents(Profile *prof)
{
	int ret = TRUE;
	int count = 0;

	if (prof->dbus_ents && kernel_supports_dbus) {
		struct dbus_entry *entry;

		list_for_each(prof->dbus_ents, entry) {
			if (!process_dbus_entry(prof->policy.rules, entry))
				ret = FALSE;
			count++;
		}
	} else if (prof->dbus_ents && !kernel_supports_dbus)
		pwarn("profile %s dbus rules not enforced\n", prof->name);

	prof->policy.count += count;
	return ret;
}

int post_process_policydb_ents(Profile *prof)
{
	if (!post_process_mnt_ents(prof))
		return FALSE;
	if (!post_process_dbus_ents(prof))
		return FALSE;

	return TRUE;
}

int process_profile_policydb(Profile *prof)
{
	int error = -1;

	prof->policy.rules = aare_new_ruleset(0);
	if (!prof->policy.rules)
		goto out;

	if (!post_process_policydb_ents(prof))
		goto out;

	if (prof->policy.count > 0) {
		prof->policy.dfa = aare_create_dfa(prof->policy.rules,
						  &prof->policy.size,
						  dfaflags);
		aare_delete_ruleset(prof->policy.rules);
		prof->policy.rules = NULL;
		if (!prof->policy.dfa)
			goto out;
	}

	aare_reset_matchflags();

	error = 0;

out:
	return error;
}

void reset_regex(void)
{
	aare_reset_matchflags();
}

#ifdef UNIT_TEST

#include "unit_test.h"

static int test_filter_slashes(void)
{
	int rc = 0;
	char *test_string;

	test_string = strdup("///foo//////f//oo////////////////");
	filter_slashes(test_string);
	MY_TEST(strcmp(test_string, "/foo/f/oo/") == 0, "simple tests");

	test_string = strdup("/foo/f/oo");
	filter_slashes(test_string);
	MY_TEST(strcmp(test_string, "/foo/f/oo") == 0, "simple test for no changes");

	test_string = strdup("/");
	filter_slashes(test_string);
	MY_TEST(strcmp(test_string, "/") == 0, "simple test for '/'");

	test_string = strdup("");
	filter_slashes(test_string);
	MY_TEST(strcmp(test_string, "") == 0, "simple test for ''");

	test_string = strdup("//usr");
	filter_slashes(test_string);
	MY_TEST(strcmp(test_string, "//usr") == 0, "simple test for // namespace");

	test_string = strdup("//");
	filter_slashes(test_string);
	MY_TEST(strcmp(test_string, "//") == 0, "simple test 2 for // namespace");

	test_string = strdup("///usr");
	filter_slashes(test_string);
	MY_TEST(strcmp(test_string, "/usr") == 0, "simple test for ///usr");

	test_string = strdup("///");
	filter_slashes(test_string);
	MY_TEST(strcmp(test_string, "/") == 0, "simple test for ///");

	test_string = strdup("/a/");
	filter_slashes(test_string);
	MY_TEST(strcmp(test_string, "/a/") == 0, "simple test for /a/");

	return rc;
}

#define MY_REGEX_TEST(input, expected_str, expected_type)						\
	do {												\
		std::string tbuf;									\
		std::string tbuf2 = "testprefix";							\
		char *output_string = NULL;								\
		std::string expected_str2;								\
		pattern_t ptype;									\
		int pos;										\
													\
		ptype = convert_aaregex_to_pcre((input), 0, tbuf, &pos);				\
		asprintf(&output_string, "simple regex conversion for '%s'\texpected = '%s'\tresult = '%s'", \
				(input), (expected_str), tbuf.c_str());					\
		MY_TEST(strcmp(tbuf.c_str(), (expected_str)) == 0, output_string);			\
		MY_TEST(ptype == (expected_type), "simple regex conversion type check for '" input "'"); \
		free(output_string);									\
		/* ensure convert_aaregex_to_pcre appends only to passed ref string */			\
		expected_str2 = tbuf2;									\
		expected_str2.append((expected_str));							\
		ptype = convert_aaregex_to_pcre((input), 0, tbuf2, &pos);				\
		asprintf(&output_string, "simple regex conversion for '%s'\texpected = '%s'\tresult = '%s'", \
				(input), expected_str2.c_str(), tbuf2.c_str());				\
		MY_TEST((tbuf2 == expected_str2), output_string);					\
		free(output_string);									\
	}												\
	while (0)

#define MY_REGEX_FAIL_TEST(input)						\
	do {												\
		std::string tbuf;									\
		pattern_t ptype;									\
		int pos;										\
													\
		ptype = convert_aaregex_to_pcre((input), 0, tbuf, &pos);				\
		MY_TEST(ptype == ePatternInvalid, "simple regex conversion invalid type check for '" input "'"); \
	}												\
	while (0)

static int test_aaregex_to_pcre(void)
{
	int rc = 0;

	MY_REGEX_TEST("/most/basic/test", "/most/basic/test", ePatternBasic);

	MY_REGEX_FAIL_TEST("\\");
	MY_REGEX_TEST("\\\\", "\\\\", ePatternBasic);
	MY_REGEX_TEST("\\blort", "blort", ePatternBasic);
	MY_REGEX_TEST("\\\\blort", "\\\\blort", ePatternBasic);
	MY_REGEX_FAIL_TEST("blort\\");
	MY_REGEX_TEST("blort\\\\", "blort\\\\", ePatternBasic);
	MY_REGEX_TEST("*", "[^/\\x00]*", ePatternRegex);
	MY_REGEX_TEST("blort*", "blort[^/\\x00]*", ePatternRegex);
	MY_REGEX_TEST("*blort", "[^/\\x00]*blort", ePatternRegex);
	MY_REGEX_TEST("\\*", "\\*", ePatternBasic);
	MY_REGEX_TEST("blort\\*", "blort\\*", ePatternBasic);
	MY_REGEX_TEST("\\*blort", "\\*blort", ePatternBasic);

	/* simple quoting */
	MY_REGEX_TEST("\\[", "\\[", ePatternBasic);
	MY_REGEX_TEST("\\]", "\\]", ePatternBasic);
	MY_REGEX_TEST("\\?", "?", ePatternBasic);
	MY_REGEX_TEST("\\{", "\\{", ePatternBasic);
	MY_REGEX_TEST("\\}", "\\}", ePatternBasic);
	MY_REGEX_TEST("\\,", ",", ePatternBasic);
	MY_REGEX_TEST("^", "\\^", ePatternBasic);
	MY_REGEX_TEST("$", "\\$", ePatternBasic);
	MY_REGEX_TEST(".", "\\.", ePatternBasic);
	MY_REGEX_TEST("+", "\\+", ePatternBasic);
	MY_REGEX_TEST("|", "\\|", ePatternBasic);
	MY_REGEX_TEST("(", "\\(", ePatternBasic);
	MY_REGEX_TEST(")", "\\)", ePatternBasic);
	MY_REGEX_TEST("\\^", "\\^", ePatternBasic);
	MY_REGEX_TEST("\\$", "\\$", ePatternBasic);
	MY_REGEX_TEST("\\.", "\\.", ePatternBasic);
	MY_REGEX_TEST("\\+", "\\+", ePatternBasic);
	MY_REGEX_TEST("\\|", "\\|", ePatternBasic);
	MY_REGEX_TEST("\\(", "\\(", ePatternBasic);
	MY_REGEX_TEST("\\)", "\\)", ePatternBasic);

	/* simple character class tests */
	MY_REGEX_TEST("[blort]", "[blort]", ePatternRegex);
	MY_REGEX_FAIL_TEST("[blort");
	MY_REGEX_FAIL_TEST("b[lort");
	MY_REGEX_FAIL_TEST("blort[");
	MY_REGEX_FAIL_TEST("blort]");
	MY_REGEX_FAIL_TEST("blo]rt");
	MY_REGEX_FAIL_TEST("]blort");
	MY_REGEX_TEST("b[lor]t", "b[lor]t", ePatternRegex);

	/* simple alternation tests */
	MY_REGEX_TEST("{alpha,beta}", "(alpha|beta)", ePatternRegex);
	MY_REGEX_TEST("baz{alpha,beta}blort", "baz(alpha|beta)blort", ePatternRegex);
	MY_REGEX_FAIL_TEST("{beta}");
	MY_REGEX_FAIL_TEST("biz{beta");
	MY_REGEX_FAIL_TEST("biz}beta");
	MY_REGEX_FAIL_TEST("biz{be,ta");
	MY_REGEX_FAIL_TEST("biz,be}ta");
	MY_REGEX_FAIL_TEST("biz{}beta");

	/* nested alternations */
	MY_REGEX_TEST("{{alpha,blort,nested},beta}", "((alpha|blort|nested)|beta)", ePatternRegex);
	MY_REGEX_FAIL_TEST("{{alpha,blort,nested}beta}");
	MY_REGEX_TEST("{{alpha,{blort,nested}},beta}", "((alpha|(blort|nested))|beta)", ePatternRegex);
	MY_REGEX_TEST("{{alpha,alpha{blort,nested}}beta,beta}", "((alpha|alpha(blort|nested))beta|beta)", ePatternRegex);
	MY_REGEX_TEST("{{alpha,alpha{blort,nested}}beta,beta}", "((alpha|alpha(blort|nested))beta|beta)", ePatternRegex);
	MY_REGEX_TEST("{{a,b{c,d}}e,{f,{g,{h{i,j,k},l}m},n}o}", "((a|b(c|d))e|(f|(g|(h(i|j|k)|l)m)|n)o)", ePatternRegex);
	/* max nesting depth = 50 */
	MY_REGEX_TEST("{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a,b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b}b,blort}",
			"(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a(a|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)|b)b|blort)", ePatternRegex);
	MY_REGEX_FAIL_TEST("{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a{a,b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b},b}b,blort}");

	/* simple single char */
	MY_REGEX_TEST("blor?t", "blor[^/\\x00]t", ePatternRegex);

	/* simple globbing */
	MY_REGEX_TEST("/*", "/[^/\\x00][^/\\x00]*", ePatternRegex);
	MY_REGEX_TEST("/blort/*", "/blort/[^/\\x00][^/\\x00]*", ePatternRegex);
	MY_REGEX_TEST("/*/blort", "/[^/\\x00][^/\\x00]*/blort", ePatternRegex);
	MY_REGEX_TEST("/*/", "/[^/\\x00][^/\\x00]*/", ePatternRegex);
	MY_REGEX_TEST("/**", "/[^/\\x00][^\\x00]*", ePatternTailGlob);
	MY_REGEX_TEST("/blort/**", "/blort/[^/\\x00][^\\x00]*", ePatternTailGlob);
	MY_REGEX_TEST("/**/blort", "/[^/\\x00][^\\x00]*/blort", ePatternRegex);
	MY_REGEX_TEST("/**/", "/[^/\\x00][^\\x00]*/", ePatternRegex);

	/* more complicated quoting */
	MY_REGEX_FAIL_TEST("\\\\[");
	MY_REGEX_FAIL_TEST("\\\\]");
	MY_REGEX_TEST("\\\\?", "\\\\[^/\\x00]", ePatternRegex);
	MY_REGEX_FAIL_TEST("\\\\{");
	MY_REGEX_FAIL_TEST("\\\\}");
	MY_REGEX_TEST("\\\\,", "\\\\,", ePatternBasic);
	MY_REGEX_TEST("\\\\^", "\\\\\\^", ePatternBasic);
	MY_REGEX_TEST("\\\\$", "\\\\\\$", ePatternBasic);
	MY_REGEX_TEST("\\\\.", "\\\\\\.", ePatternBasic);
	MY_REGEX_TEST("\\\\+", "\\\\\\+", ePatternBasic);
	MY_REGEX_TEST("\\\\|", "\\\\\\|", ePatternBasic);
	MY_REGEX_TEST("\\\\(", "\\\\\\(", ePatternBasic);
	MY_REGEX_TEST("\\\\)", "\\\\\\)", ePatternBasic);

	/* more complicated character class tests */
	/*   -- embedded alternations */
	MY_REGEX_TEST("b[\\lor]t", "b[lor]t", ePatternRegex);
	MY_REGEX_TEST("b[{a,b}]t", "b[{a,b}]t", ePatternRegex);
	MY_REGEX_TEST("{alpha,b[{a,b}]t,gamma}", "(alpha|b[{a,b}]t|gamma)", ePatternRegex);

	/* pcre will ignore the '\' before '\{', but it should be okay
	 * for us to pass this on to pcre as '\{' */
	MY_REGEX_TEST("b[\\{a,b\\}]t", "b[\\{a,b\\}]t", ePatternRegex);
	MY_REGEX_TEST("{alpha,b[\\{a,b\\}]t,gamma}", "(alpha|b[\\{a,b\\}]t|gamma)", ePatternRegex);
	MY_REGEX_TEST("{alpha,b[\\{a\\,b\\}]t,gamma}", "(alpha|b[\\{a\\,b\\}]t|gamma)", ePatternRegex);

	return rc;
}

int main(void)
{
	int rc = 0;
	int retval;

	retval = test_filter_slashes();
	if (retval != 0)
		rc = retval;

	retval = test_aaregex_to_pcre();
	if (retval != 0)
		rc = retval;

	return rc;
}
#endif /* UNIT_TEST */
