/*
 *  rdfxml_parser.c
 *
 *  $Id$
 *
 *  RDF/XML parser
 *
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *
 *  Copyright (C) 1998-2006 OpenLink Software
 *
 *  This project is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; only version 2 of the License, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "Dk.h"
#include "rdf_core.h"
#include "sqlnode.h"
#include "xml.h"
#include "xmltree.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "xmlparser.h"
#include "xmlparser_impl.h"
#ifdef __cplusplus
}
#endif

#ifdef RDFXML_DEBUG
#define rdfxml_dbg_printf(x) dbg_printf (x)
#else
#define rdfxml_dbg_printf(x)
#endif

/* Part 1. RDF/XML-specific functions */

#define XRL_SET_INHERITABLE(xrl,name,value,errmsg) do { \
    if (xrl->name##_set) \
      { \
        dk_free_tree ((value)); \
        xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 200, errmsg); \
        return ;\
      } \
    xrl->name = (value); \
    xrl->name##_set = 1; \
  } while (0)

#define XRL_SET_NONINHERITABLE(xrl,name,value,errmsg) do { \
    if (NULL != xrl->name) \
      { \
        dk_free_tree ((value)); \
        xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 200, errmsg); \
        return ;\
      } \
    xrl->name = (value); \
  } while (0)

void
xp_rdfxml_get_name_parts (xp_node_t * xn, char * name, int use_default, caddr_t *nsuri_ret, char **local_ret)
{
  xp_node_t * ctx_xn;
  size_t ns_len = 0, nsinx;
  char * local = strrchr (name, ':');
  if (!local && !use_default)
    {
      nsuri_ret[0] = uname___empty;
      local_ret[0] = name;
      return;
    }
  if (local)
    {
      ns_len = local - name;
      if (bx_std_ns_pref (name, ns_len))
        {
          nsuri_ret[0] = uname_xml;
          local_ret[0] = local + 1;
	  return;
        }
      local++;
    }
  else
    {
      ns_len = 0;
      local = name;
    }
  ctx_xn = xn;
  while (ctx_xn)
    {
      size_t n_ns = BOX_ELEMENTS_0 (ctx_xn->xn_namespaces);
      for (nsinx = 0; nsinx < n_ns; nsinx += 2)
	{
	  char *ctxname = ctx_xn->xn_namespaces[nsinx];
	  if ((box_length (ctxname) == (ns_len + 1)) && !memcmp (ctxname, name, ns_len))
	    {
	      char * ns_uri = ctx_xn->xn_namespaces[nsinx + 1];
              nsuri_ret[0] = ns_uri;
              local_ret[0] = local;
              return;
            }
	}
      ctx_xn = ctx_xn->xn_parent;
    }
  nsuri_ret[0] = uname___empty;
  local_ret[0] = name;
  if (0 != ns_len)
    xmlparser_logprintf (xn->xn_xp->xp_parser, XCFG_FATAL, 100+strlen (name), "Name '%.1000s' contains undefined namespace prefix", name);
}


void xp_pop_rdf_locals (xparse_ctx_t *xp)
{
  xp_rdfxml_locals_t *inner = xp->xp_rdfxml_locals;
  if (inner->xrl_base_set)
    dk_free_tree (inner->xrl_base);
  if (NULL != inner->xrl_datatype)
    dk_free_tree (inner->xrl_datatype);
  if (inner->xrl_language_set)
    dk_free_tree (inner->xrl_language);
  dk_free_box (inner->xrl_predicate);
  if (NULL != inner->xrl_subject)
    dk_free_tree (inner->xrl_subject);
  if (NULL != inner->xrl_reification_id)
    dk_free_tree (inner->xrl_reification_id);
  while (NULL != inner->xrl_seq_items)
    dk_free_tree (dk_set_pop (&(inner->xrl_seq_items)));
  xp->xp_rdfxml_locals = inner->xrl_parent;
  memset (inner, -1, sizeof (xp_rdfxml_locals_t));
  inner->xrl_parent = xp->xp_rdfxml_free_list;
  xp->xp_rdfxml_free_list = inner;
}


xp_rdfxml_locals_t *xp_push_rdf_locals (xparse_ctx_t *xp)
{
  xp_rdfxml_locals_t *outer = xp->xp_rdfxml_locals;
  xp_rdfxml_locals_t *inner;
  if (NULL != xp->xp_rdfxml_free_list)
    {
      inner = xp->xp_rdfxml_free_list;
      xp->xp_rdfxml_free_list = inner->xrl_parent;
    }
  else
    inner = dk_alloc (sizeof (xp_rdfxml_locals_t));
  memset (inner, 0, sizeof (xp_rdfxml_locals_t));
  inner->xrl_base = outer->xrl_base;
  inner->xrl_language = outer->xrl_language;
  inner->xrl_parent = outer;
  xp->xp_rdfxml_locals = inner;
  return inner;
}


caddr_t
xp_rdfxml_resolved_iid (xparse_ctx_t *xp, const char *avalue, int is_id_attr)
{
  caddr_t err = NULL;
  caddr_t local, res;
  if (is_id_attr)
    {
      local = dk_alloc_box (2 + strlen (avalue), DV_STRING);
      local[0] = '#';
      strcpy (local+1, avalue);
    }
  else
    local = box_dv_short_string (avalue);
  res = xml_uri_resolve_like_get (xp->xp_qi, &err, xp->xp_rdfxml_locals->xrl_base, local, "UTF-8");
  dk_free_box (local);
  if (NULL != err)
    sqlr_resignal (err);
#ifdef MALLOC_DEBUG
  dk_check_tree (res);
#endif
  return res;
}


caddr_t
xp_rdfxml_bnode_iid (xparse_ctx_t *xp, caddr_t avalue)
{
  caddr_t res;
  rdfxml_dbg_printf (("\nxp_rdfxml_bnode_iid (\"%s\")", avalue));
  res = tf_bnode_iid (xp->xp_tf, avalue);
#ifdef MALLOC_DEBUG
  dk_check_tree (res);
#endif
  return res;
}


void
xp_rdfxml_triple (xparse_ctx_t *xp, caddr_t s, caddr_t p, caddr_t o)
{
#ifdef MALLOC_DEBUG
  dk_check_tree (s);
  dk_check_tree (p);
  dk_check_tree (o);
#endif
  rdfxml_dbg_printf (("\nxp_rdfxml_triple (\"%s\", \"%s\", \"%s\")", s, p, o));
  tf_triple (xp->xp_tf, s, p, o);
}


void
xp_rdfxml_triple_l (xparse_ctx_t *xp, caddr_t s, caddr_t p, caddr_t o, caddr_t dt, caddr_t lang)
{
#ifdef MALLOC_DEBUG
  dk_check_tree (s);
  dk_check_tree (p);
  dk_check_tree (o);
  dk_check_tree (dt);
  dk_check_tree (lang);
#endif
  rdfxml_dbg_printf (("\nxp_rdfxml_triple (\"%s\", \"%s\", \"%s\", \"%s\", \"%s\")", s, p, o, dt, lang));
  tf_triple_l (xp->xp_tf, s, p, o, dt, lang);
}


/*#define RECOVER_RDF_VALUE 1*/

void
xp_rdfxml_element (void *userdata, char * name, vxml_parser_attrdata_t *attrdata)
{
  xparse_ctx_t * xp = (xparse_ctx_t*) userdata;
  xp_rdfxml_locals_t *outer = xp->xp_rdfxml_locals;
  xp_rdfxml_locals_t *inner;
  xp_node_t *xn;
  caddr_t subj_type = NULL;
  int inx, fill, n_attrs, n_ns;
  dk_set_t inner_attr_props = NULL;
  caddr_t tmp_nsuri;
  char * tmp_local;
#ifdef RECOVER_RDF_VALUE
  caddr_t rdf_val = NULL;
#endif
  if (XRL_PARSETYPE_LITERAL == outer->xrl_parsetype)
    {
      xp_element (userdata, name, attrdata);
      return;
    }
  else if (XRL_PARSETYPE_EMPTYPROP == outer->xrl_parsetype)
    xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Sub-element in a predicate element with object node attribute");
  inner = xp_push_rdf_locals (xp);
  xn = xp->xp_free_list;
  if (NULL == xn)
    xn = dk_alloc (sizeof (xp_node_t));
  else
    xp->xp_free_list = xn->xn_parent;
  memset (xn, 0, sizeof (xp_node_t));
  xn->xn_xp = xp;
  xn->xn_parent = xp->xp_current;
  xp->xp_current = xn;
#ifdef DEBUG
  if (NULL != xp->xp_boxed_name)
    GPF_T1("Memory leak in xp->xp_boxed_name");
#endif
  inner->xrl_xn = xn;
  n_ns = attrdata->local_nsdecls_count;
  if (n_ns)
    {
      caddr_t *save_ns = (caddr_t*) dk_alloc_box (2 * n_ns * sizeof (caddr_t), DV_ARRAY_OF_POINTER);
      /* Trick here: xn->xn_attrs is set to xn->xn_namespaces in order to free memory on errors or element end. */
      xn->xn_attrs = xn->xn_namespaces = save_ns;
      fill = 0;
      for (inx = 0; inx < n_ns; inx++)
        {
          save_ns[fill++] = box_dv_uname_string (attrdata->local_nsdecls[inx].nsd_prefix);
          save_ns[fill++] = box_dv_uname_string (attrdata->local_nsdecls[inx].nsd_uri);
        }
    }
  xp_rdfxml_get_name_parts (xn, name, 1, &tmp_nsuri, &tmp_local);
  if (!strcmp ("http://www.w3.org/1999/02/22-rdf-syntax-ns#", tmp_nsuri))
    {
      if (!strcmp ("RDF", tmp_local))
        {
          if (XRL_PARSETYPE_TOP_LEVEL != outer->xrl_parsetype)
            xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 200, "Element rdf:RDF can appear only at top level");
          inner->xrl_parsetype = XRL_PARSETYPE_RESOURCE;
        }
      else if (!strcmp ("Description", tmp_local))
        {
          if (XRL_PARSETYPE_PROPLIST == outer->xrl_parsetype)
            xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 200, "Element rdf:Description can not appear in list of properties");
          inner->xrl_parsetype = XRL_PARSETYPE_PROPLIST;
        }
      else if (XRL_PARSETYPE_PROPLIST == outer->xrl_parsetype)
        {
          caddr_t full_element_name;
          if (!strcmp ("li", tmp_local))
            {
              int li_count = ++(outer->xrl_li_count);
              full_element_name = box_sprintf (100, "http://www.w3.org/1999/02/22-rdf-syntax-ns#_%d", li_count);
            }
          else
            {
              size_t l1 = strlen (tmp_nsuri), l2 = strlen (tmp_local);
              full_element_name = dk_alloc_box (l1 + l2 + 1, DV_STRING);
              memcpy (full_element_name, tmp_nsuri, l1);
              strcpy (full_element_name + l1, tmp_local);
            }
          dk_free_tree (inner->xrl_predicate);
          inner->xrl_predicate = full_element_name;
          inner->xrl_parsetype = XRL_PARSETYPE_RES_OR_LIT;
        }
#if 0
      else if (!strcmp ("Seq", tmp_local))
        {
          xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 200, "RDF/XML parser of Virtuoso does not support rdf:Seq syntax");
          return;
        }
#endif
      else if (
        !strcmp ("Property", tmp_local) ||
        !strcmp ("Bag", tmp_local) ||
        !strcmp ("Seq", tmp_local) ||
        !strcmp ("Alt", tmp_local)  ||
        !strcmp ("List", tmp_local) ||
        !strcmp ("Statement", tmp_local) )
        {
          size_t l1 = strlen (tmp_nsuri), l2 = strlen (tmp_local);
          caddr_t full_element_name = dk_alloc_box (l1 + l2 + 1, DV_STRING);
          memcpy (full_element_name, tmp_nsuri, l1);
          strcpy (full_element_name + l1, tmp_local);
          subj_type = xp->xp_boxed_name = full_element_name;
          inner->xrl_parsetype = XRL_PARSETYPE_PROPLIST;
        }
      else
        {
          xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 200, "Unknown element in RDF namespace");
          return;
        }
    }
  else
    {
      size_t l1 = strlen (tmp_nsuri), l2 = strlen (tmp_local);
      caddr_t full_element_name = dk_alloc_box (l1 + l2 + 1, DV_STRING);
      memcpy (full_element_name, tmp_nsuri, l1);
      strcpy (full_element_name + l1, tmp_local);
      if (XRL_PARSETYPE_PROPLIST == outer->xrl_parsetype)
        {
          dk_free_tree (inner->xrl_predicate);
          inner->xrl_predicate = full_element_name;
          inner->xrl_parsetype = XRL_PARSETYPE_RES_OR_LIT;
        }
      else
        {
          subj_type = xp->xp_boxed_name = full_element_name;
          inner->xrl_parsetype = XRL_PARSETYPE_PROPLIST;
        }
    }
  n_attrs = attrdata->local_attrs_count;
  /* we do one loop first to see if there are xml:base, xml:lang or xml:space then rest */
  for (inx = 0; inx < n_attrs; inx ++)
    {
      char *raw_aname = attrdata->local_attrs[inx].ta_raw_name.lm_memblock;
      caddr_t avalue = attrdata->local_attrs[inx].ta_value;
      xp_rdfxml_get_name_parts (xn, raw_aname, 0, &tmp_nsuri, &tmp_local);
      if (!stricmp (tmp_nsuri, "xml"))
        {
          if (!strcmp (tmp_local, "lang"))
            XRL_SET_INHERITABLE (inner, xrl_language, box_dv_short_string (avalue), "Attribute 'xml:lang' is used twice");
          else if (!strcmp (tmp_local, "base"))
            XRL_SET_INHERITABLE (inner, xrl_base, box_dv_short_string (avalue), "Attribute 'xml:base' is used twice");
          else if (0 != strcmp (tmp_local, "space"))
            xmlparser_logprintf (xp->xp_parser, XCFG_WARNING, 200,
              "Unsupported 'xml:...' attribute, only 'xml:lang', 'xml:base' and 'xml:space' are supported" );
	}
    }
  for (inx = 0; inx < n_attrs; inx ++)
    {
      char *raw_aname = attrdata->local_attrs[inx].ta_raw_name.lm_memblock;
      caddr_t avalue = attrdata->local_attrs[inx].ta_value;
      xp_rdfxml_get_name_parts (xn, raw_aname, 0, &tmp_nsuri, &tmp_local);
      if (!strcmp (tmp_nsuri, "http://www.w3.org/1999/02/22-rdf-syntax-ns#"))
        {
          if (!strcmp (tmp_local, "about"))
            {
              caddr_t inner_subj;
              if (XRL_PARSETYPE_PROPLIST == outer->xrl_parsetype)
                {
                  xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Attribute 'rdf:about' can not appear in element that is supposed to be property name");
                  return;
                }
              inner_subj = xp_rdfxml_resolved_iid (xp, avalue, 0);
              XRL_SET_NONINHERITABLE (inner, xrl_subject, inner_subj, "Attribute 'rdf:about' conflicts with other attribute that set the subject");
              inner->xrl_parsetype = XRL_PARSETYPE_PROPLIST;
            }
          else if (!strcmp (tmp_local, "resource"))
            {
              caddr_t inner_subj;
              if (XRL_PARSETYPE_PROPLIST != outer->xrl_parsetype)
                {
                  xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Attribute 'rdf:resource' can appear only in element that is supposed to be property name");
                  return;
                }
              inner_subj = xp_rdfxml_resolved_iid (xp, avalue, 0);
              XRL_SET_NONINHERITABLE (inner, xrl_subject, inner_subj, "Attribute 'rdf:resource' conflicts with other attribute that set the subject");
              inner->xrl_parsetype = XRL_PARSETYPE_EMPTYPROP;
            }
          else if (!strcmp (tmp_local, "nodeID"))
            {
              caddr_t inner_subj = xp_rdfxml_bnode_iid (xp, box_dv_short_string (avalue));
              XRL_SET_NONINHERITABLE (inner, xrl_subject, inner_subj, "Attribute 'rdf:nodeID' conflicts with other attribute that set the subject");
              if (XRL_PARSETYPE_PROPLIST == outer->xrl_parsetype)
                {
                  inner->xrl_parsetype = XRL_PARSETYPE_EMPTYPROP;
                }
              else
                {
                  inner->xrl_parsetype = XRL_PARSETYPE_PROPLIST;
                }
            }
          else if (!strcmp (tmp_local, "ID"))
            {
              if (XRL_PARSETYPE_PROPLIST == outer->xrl_parsetype)
                {
                  caddr_t reif_subj = xp_rdfxml_resolved_iid (xp, avalue, 1);
                  XRL_SET_NONINHERITABLE (inner, xrl_reification_id, reif_subj, "Reification ID of the statement is set twice by 'rdf:ID' attribute of a property element");
                }
              else
                {
                  caddr_t inner_subj = xp_rdfxml_resolved_iid (xp, avalue, 1);
                  XRL_SET_NONINHERITABLE (inner, xrl_subject, inner_subj, "Attribute 'rdf:ID' conflicts with other attribute that set node ID");
                  inner->xrl_parsetype = XRL_PARSETYPE_PROPLIST;
                }
            }
          else if (!strcmp (tmp_local, "datatype"))
            {
              if (XRL_PARSETYPE_PROPLIST != outer->xrl_parsetype)
                {
                  xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Attribute 'rdf:datatype' can appear only in property elements");
                  return;
                }
              XRL_SET_NONINHERITABLE (inner, xrl_datatype, xp_rdfxml_resolved_iid (xp, avalue, 0),  "Attribute 'rdf:datatype' us used twice");
              inner->xrl_parsetype = XRL_PARSETYPE_LITERAL;
            }
          else if (!strcmp (tmp_local, "parseType"))
            {
              if (XRL_PARSETYPE_PROPLIST != outer->xrl_parsetype)
                {
                  xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Attribute 'rdf:parseType' can appear only in property elements");
                  return;
                }
              if (!strcmp (avalue, "Resource"))
                {
                  caddr_t inner_subj = xp_rdfxml_bnode_iid (xp, NULL);
                  XRL_SET_NONINHERITABLE (inner, xrl_subject, inner_subj, "Attribute parseType='Resource' can not be used if object is set by other attribute");
                  inner->xrl_parsetype = XRL_PARSETYPE_PROPLIST;
                }
              else if (!strcmp (avalue, "Literal"))
                {
                  inner->xrl_parsetype = XRL_PARSETYPE_LITERAL;
                }
              else if (!strcmp (avalue, "Collection"))
                {
                  inner->xrl_parsetype = XRL_PARSETYPE_COLLECTION;
                  return;
                }
              else
                {
                  xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Unknown parseType");
                  return;
                }
            }
	  else if (!strcmp (tmp_local, "type"))
	    {
              goto push_inner_attr_prop; /* see below */
	    }
	  else if (!strcmp (tmp_local, "value"))
	    {
#ifdef RECOVER_RDF_VALUE
	      rdf_val = avalue;
#else
              goto push_inner_attr_prop; /* see below */
#endif
	    }
          else
            {
              xmlparser_logprintf (xp->xp_parser, XCFG_WARNING, 200,
                "Unsupported 'rdf:...' attribute" );
                goto push_inner_attr_prop; /* see below */
            }
          continue;
        }
      else if (!stricmp (tmp_nsuri, "xml"))
        {
/*
   	  XXX: moved above
          if (!strcmp (tmp_local, "lang"))
            XRL_SET_INHERITABLE (inner, xrl_language, box_dv_short_string (avalue), "Attribute 'xml:lang' is used twice");
          else if (!strcmp (tmp_local, "base"))
            XRL_SET_INHERITABLE (inner, xrl_base, box_dv_short_string (avalue), "Attribute 'xml:base' is used twice");
          else if (0 != strcmp (tmp_local, "space"))
            xmlparser_logprintf (xp->xp_parser, XCFG_WARNING, 200,
              "Unsupported 'xml:...' attribute, only 'xml:lang', 'xml:base' and 'xml:space' are supported" );
*/
          continue;
        }
push_inner_attr_prop:
      dk_set_push (&inner_attr_props, avalue);
      dk_set_push (&inner_attr_props, tmp_local);
      dk_set_push (&inner_attr_props, tmp_nsuri);
      inner->xrl_parsetype = XRL_PARSETYPE_PROPLIST;
    }
  if ((NULL != inner->xrl_subject) || (NULL != inner_attr_props))
    {
      if (XRL_PARSETYPE_LITERAL == inner->xrl_parsetype)
        {
          xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 200,
            "Conflicting attributes: property value can not be a node and a literal simultaneously" );
          return;
        }
    }
/*  if ((XRL_PARSETYPE_PROPLIST == outer->xrl_parsetype) && (NULL != outer->xrl_subject))
    XRL_SET_NONINHERITABLE (inner, xrl_subject, box_copy_tree (outer->xrl_subject));
*/
  if (NULL == inner->xrl_subject)
    {
      if ((NULL != inner_attr_props) || (NULL != subj_type) ||
#ifdef RECOVER_RDF_VALUE
        (NULL != rdf_val) ||
#endif
        (XRL_PARSETYPE_PROPLIST == inner->xrl_parsetype) )
        {
          caddr_t inner_subj = xp_rdfxml_bnode_iid (xp, NULL);
          XRL_SET_NONINHERITABLE (inner, xrl_subject, inner_subj, "Blank node object can not be defined here");
          inner->xrl_parsetype = XRL_PARSETYPE_PROPLIST;
        }
    }
  if ((XRL_PARSETYPE_PROPLIST == inner->xrl_parsetype) && (NULL != outer->xrl_predicate))
    XRL_SET_NONINHERITABLE (outer, xrl_subject, box_copy_tree (inner->xrl_subject), "A property can not have two object values");
  if (NULL != subj_type)
    xp_rdfxml_triple (xp, inner->xrl_subject, uname_rdf_ns_uri_type, subj_type);
#ifdef RECOVER_RDF_VALUE
  if (NULL != rdf_val)
    { /* This preserves semantics */
      caddr_t resolved_rdf_val = xp_rdfxml_resolved_iid (xp, rdf_val, 0);
      xp_rdfxml_triple (xp, inner->xrl_subject, uname_rdf_ns_uri_value, resolved_rdf_val);
      dk_free_box (resolved_rdf_val);
    }
#endif
  if (NULL != xp->xp_boxed_name)
    {
      dk_free_box (xp->xp_boxed_name);
      xp->xp_boxed_name = NULL;
    }
  while (NULL != inner_attr_props)
    {
      size_t l1, l2;
      caddr_t aname, avalue;
      tmp_nsuri = dk_set_pop (&inner_attr_props);
      tmp_local = dk_set_pop (&inner_attr_props);
      avalue = dk_set_pop (&inner_attr_props);
      l1 = strlen (tmp_nsuri);
      l2 = strlen (tmp_local);
      xp->xp_boxed_name = aname = dk_alloc_box (l1 + l2 + 1, DV_STRING);
      memcpy (aname, tmp_nsuri, l1);
      strcpy (aname + l1, tmp_local);
      xp_rdfxml_triple_l (xp, inner->xrl_subject, aname, avalue, NULL, NULL);
      dk_free_box (aname);
      xp->xp_boxed_name = NULL;
    }
  if ((XRL_PARSETYPE_PROPLIST == inner->xrl_parsetype) && (XRL_PARSETYPE_PROPLIST == outer->xrl_parsetype))
    { /* This means parseType="Resource". It should be handled immediately to prevent error in case of parseType="Resource" nested inside inner. */
      xp_rdfxml_triple (xp, outer->xrl_subject, inner->xrl_predicate, inner->xrl_subject);
      if (NULL != inner->xrl_reification_id)
        {
          xp_rdfxml_triple (xp, inner->xrl_reification_id, uname_rdf_ns_uri_subject, outer->xrl_subject);
          xp_rdfxml_triple (xp, inner->xrl_reification_id, uname_rdf_ns_uri_predicate, inner->xrl_predicate);
          xp_rdfxml_triple (xp, inner->xrl_reification_id, uname_rdf_ns_uri_object, inner->xrl_subject);
          xp_rdfxml_triple (xp, inner->xrl_reification_id, uname_rdf_ns_uri_type, uname_rdf_ns_uri_Statement);
        }
      dk_free_tree (inner->xrl_predicate);
      inner->xrl_predicate = NULL;
    }
}


void
xp_rdfxml_element_end (void *userdata, const char * name)
{
  xparse_ctx_t *xp = (xparse_ctx_t*) userdata;
  xp_rdfxml_locals_t *inner = xp->xp_rdfxml_locals;
  if (XRL_PARSETYPE_LITERAL != inner->xrl_parsetype)
    {
      xp_node_t *current_node = xp->xp_current;
      xp_node_t *parent_node = xp->xp_current->xn_parent;
      xp_rdfxml_locals_t *outer = inner->xrl_parent;
      if ((NULL != outer) && (XRL_PARSETYPE_COLLECTION == outer->xrl_parsetype))
        {
          xp_rdfxml_locals_t *outer = inner->xrl_parent;
          caddr_t subj;
          if (NULL == outer->xrl_subject)
            subj = xp_rdfxml_bnode_iid (xp, NULL);
          else
            {
              subj = outer->xrl_subject;
              outer->xrl_subject = NULL; /* To avoid double free, because subj will go to xrl_seq_items */
            }
          dk_set_push (&(outer->xrl_seq_items), subj);
        }
      else if (XRL_PARSETYPE_COLLECTION == inner->xrl_parsetype)
        {
          caddr_t tail = uname_rdf_ns_uri_nil;
          while (NULL != inner->xrl_seq_items)
            {
              caddr_t val = (caddr_t)(inner->xrl_seq_items->data);
              caddr_t node = xp_rdfxml_bnode_iid (xp, NULL);
              xp_rdfxml_triple (xp, node, uname_rdf_ns_uri_first, val);
              xp_rdfxml_triple (xp, node, uname_rdf_ns_uri_rest, tail);
              dk_free_tree (dk_set_pop (&(inner->xrl_seq_items)));
              tail = node;
            }
          xp_rdfxml_triple (xp, outer->xrl_subject, inner->xrl_predicate, tail);
        }
      else if (NULL != inner->xrl_predicate)
        {
          xp_rdfxml_locals_t *outer = inner->xrl_parent;
          if (NULL == inner->xrl_subject)
            inner->xrl_subject = xp_rdfxml_bnode_iid (xp, NULL);
          xp_rdfxml_triple (xp, outer->xrl_subject, inner->xrl_predicate, inner->xrl_subject);
          if (NULL != inner->xrl_reification_id)
            {
              xp_rdfxml_triple (xp, inner->xrl_reification_id, uname_rdf_ns_uri_subject, outer->xrl_subject);
              xp_rdfxml_triple (xp, inner->xrl_reification_id, uname_rdf_ns_uri_predicate, inner->xrl_predicate);
              xp_rdfxml_triple (xp, inner->xrl_reification_id, uname_rdf_ns_uri_object, inner->xrl_subject);
              xp_rdfxml_triple (xp, inner->xrl_reification_id, uname_rdf_ns_uri_type, uname_rdf_ns_uri_Statement);
            }
        }
      if (0 != strses_length (xp->xp_strses))
        GPF_T1("xp_rdfxml_element_end(): non-empty xp_strses outside XRL_PARSETYPE_LITERAL");
      if (NULL != current_node->xn_children)
        GPF_T1("xp_rdfxml_element_end(): non-empty xn_children outside XRL_PARSETYPE_LITERAL");
      dk_free_tree (current_node->xn_attrs);
      xp->xp_current = parent_node;
      current_node->xn_parent = xp->xp_free_list;
      xp->xp_free_list = current_node;
      xp_pop_rdf_locals (xp);
      return;
    }
  if (inner->xrl_xn == xp->xp_current)
    {
      xp_node_t * current_node = xp->xp_current;
      xp_node_t * parent_node = xp->xp_current->xn_parent;
      caddr_t obj;
      xml_tree_ent_t *literal_xte;
      if (NULL == xp->xp_current->xn_children)
        {
          obj = strses_string (xp->xp_strses);
          strses_flush (xp->xp_strses);
        }
      else
        {
          dk_set_t children;
          caddr_t *literal_head;
          caddr_t literal_tree;
          XP_STRSES_FLUSH (xp);
          children = dk_set_nreverse (current_node->xn_children);
          literal_head = (caddr_t *)list (1, uname__root);
          children = CONS (literal_head, children);
          literal_tree = list_to_array (children);
          literal_xte = xte_from_tree (literal_tree, xp->xp_qi);
          obj = (caddr_t) literal_xte;
        }
      dk_free_tree (current_node->xn_attrs);
      xp->xp_current = parent_node;
      current_node->xn_parent = xp->xp_free_list;
      xp->xp_free_list = current_node;
      xp_rdfxml_triple_l (xp, inner->xrl_parent->xrl_subject, inner->xrl_predicate, obj, inner->xrl_datatype, inner->xrl_language);
      dk_free_tree (obj);
      xp_pop_rdf_locals (xp);
      return;
    }
  xp_element_end (userdata, name);
}


void
xp_rdfxml_id (void *userdata, char * name)
{
  xparse_ctx_t * xp = (xparse_ctx_t*) userdata;
  if (XRL_PARSETYPE_LITERAL == xp->xp_rdfxml_locals->xrl_parsetype)
    xp_id (userdata, name);
}


void
xp_rdfxml_character (vxml_parser_t * parser,  char * s, int len)
{
  xparse_ctx_t *xp = (xparse_ctx_t *) parser;
  switch (xp->xp_rdfxml_locals->xrl_parsetype)
    {
    case XRL_PARSETYPE_LITERAL:
      session_buffered_write (xp->xp_strses, s, len);
      break;
    case XRL_PARSETYPE_RES_OR_LIT:
      {
        char *tail = s+len;
        while ((--tail) >= s)
          if (NULL == strchr (" \t\r\n", tail[0]))
            {
              xp->xp_rdfxml_locals->xrl_parsetype = XRL_PARSETYPE_LITERAL;
              session_buffered_write (xp->xp_strses, s, len);
              break;
            }
        break;
      }
    default:
      {
        char *tail = s+len;
        while ((--tail) >= s)
          if (NULL == strchr (" \t\r\n", tail[0]))
            {
              xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Non-whitespace character found instead of XML element");
              return;
            }
        break;
      }
    }
}

void
xp_rdfxml_entity (vxml_parser_t * parser, const char * refname, int reflen, int isparam, const xml_def_4_entity_t *edef)
{
  xparse_ctx_t *xp = (xparse_ctx_t *) parser;
  switch (xp->xp_rdfxml_locals->xrl_parsetype)
    {
    case XRL_PARSETYPE_LITERAL:
      xp_entity (parser, refname, reflen, isparam, edef);
      break;
    case XRL_PARSETYPE_RES_OR_LIT:
      xp->xp_rdfxml_locals->xrl_parsetype = XRL_PARSETYPE_LITERAL;
      xp_entity (parser, refname, reflen, isparam, edef);
      break;
    default:
      xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Entity found instead of XML element");
      break;
    }
}

void
xp_rdfxml_pi (vxml_parser_t * parser, const char *target, const char *data)
{
  xparse_ctx_t *xp = (xparse_ctx_t *) parser;
  switch (xp->xp_rdfxml_locals->xrl_parsetype)
    {
    case XRL_PARSETYPE_LITERAL:
      xp_pi (parser, target, data);
      break;
    case XRL_PARSETYPE_TOP_LEVEL:
      break;
    case XRL_PARSETYPE_RES_OR_LIT:
      xp->xp_rdfxml_locals->xrl_parsetype = XRL_PARSETYPE_LITERAL;
      xp_pi (parser, target, data);
      break;
    default:
      xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Processing instruction found instead of XML element");
      break;
    }
}

void
xp_rdfxml_comment (vxml_parser_t * parser, const char *text)
{
  xparse_ctx_t *xp = (xparse_ctx_t *) parser;
  switch (xp->xp_rdfxml_locals->xrl_parsetype)
    {
    case XRL_PARSETYPE_LITERAL:
      xp_comment (parser, text);
      break;
    }
}

caddr_t default_rdf_dtd_config = NULL;

/* Part 2. RDFa-specific functions */

void
xp_expand_relative_uri (caddr_t base, caddr_t *relative_ptr)
{
  caddr_t relative = relative_ptr[0];
  caddr_t expanded;
  caddr_t err = NULL;
  if ((NULL == base) || ('\0' == base[0]) || (NULL == relative) || (DV_IRI_ID == DV_TYPE_OF (relative)) || !strncmp (relative, "http://", 7))
    return;
  expanded = rfc1808_expand_uri (/*xn->xn_xp->xp_qi,*/ base, relative, "UTF-8", 0, "UTF-8", "UTF-8", &err);
  if (NULL != err)
    {
#ifdef RDFXML_DEBUG
      GPF_T1("xp_" "expand_relative_uri(): expand_uri failed");
#else
      expanded = NULL;
#endif
    }
  if (expanded == base)
    expanded = box_copy (expanded);
  else if (expanded != relative)
    dk_free_box (relative);
  relative_ptr[0] = expanded;
}


caddr_t
xp_rdfa_expand_name (xp_node_t * xn, const char *name, const char *colon, int use_default_ns/*, caddr_t base*/)
{
  xp_node_t * ctx_xn;
  size_t ns_len = 0, nsinx;
  const char *local;
  caddr_t relative = NULL;
  if (!colon && !use_default_ns)
    {
      relative = box_dv_short_string (name);
      goto relative_is_set; /* see below */
    }
  if (NULL != colon)
    {
      ns_len = colon - name;
      local = colon + 1;
      if (bx_std_ns_pref (name, ns_len))
        return box_dv_short_strconcat (uname_xml, local);
    }
  else
    {
      ns_len = 0;
      local = name;
    }
  ctx_xn = xn;
  while (ctx_xn)
    {
      size_t n_ns = BOX_ELEMENTS_0 (ctx_xn->xn_namespaces);
      for (nsinx = 0; nsinx < n_ns; nsinx += 2)
	{
	  char *ctxname = ctx_xn->xn_namespaces[nsinx];
	  if ((box_length (ctxname) == (ns_len + 1)) && !memcmp (ctxname, name, ns_len))
	    {
	      char * ns_uri = ctx_xn->xn_namespaces[nsinx + 1];
              relative = box_dv_short_strconcat (ns_uri, local);
              goto relative_is_set; /* see below */
            }
	}
      ctx_xn = ctx_xn->xn_parent;
    }
  if (0 != ns_len)
    return NULL; /* error: undefined namespace prefix */
  relative = box_dv_short_string (name);

relative_is_set:
  /*xp_expand_relative_uri (base, &relative);*/
  return relative;
}

int
rdfa_attribute_code (const char *name)
{
  static void *names[] = {
    "about"		, (void *)((ptrlong)RDFA_ATTR_ABOUT)	,
    "content"		, (void *)((ptrlong)RDFA_ATTR_CONTENT)	,
    "datatype"		, (void *)((ptrlong)RDFA_ATTR_DATATYPE)	,
    "href"		, (void *)((ptrlong)RDFA_ATTR_HREF)	,
    "property"		, (void *)((ptrlong)RDFA_ATTR_PROPERTY)	,
    "rel"		, (void *)((ptrlong)RDFA_ATTR_REL)	,
    "resource"		, (void *)((ptrlong)RDFA_ATTR_RESOURCE)	,
    "rev"		, (void *)((ptrlong)RDFA_ATTR_REV)	,
    "src"		, (void *)((ptrlong)RDFA_ATTR_SRC)	,
    "typeof"		, (void *)((ptrlong)RDFA_ATTR_TYPEOF)	,
    "xml:base"		, (void *)((ptrlong)RDFA_ATTR_XML_BASE)	,
    "xml:lang"		, (void *)((ptrlong)RDFA_ATTR_XML_LANG)	};
  int pos = ecm_find_name (name, names, sizeof (names)/(2 * sizeof(void *)), 2 * sizeof(void *));
  if (ECM_MEM_NOT_FOUND == pos)
    return 0;
  return ((ptrlong *)names) [2 * pos + 1];
}

caddr_t
rdfa_rel_rev_value_is_reserved (const char *val)
{
  static void *vals[] = {
    "alternate"		, &uname_xhv_ns_uri_alternate	,
    "appendix"		, &uname_xhv_ns_uri_appendix	,
    "bookmark"		, &uname_xhv_ns_uri_bookmark	,
    "chapter"		, &uname_xhv_ns_uri_chapter	,
    "cite"		, &uname_xhv_ns_uri_cite	,
    "contents"		, &uname_xhv_ns_uri_contents	,
    "copyright"		, &uname_xhv_ns_uri_copyright	,
    "first"		, &uname_xhv_ns_uri_first	,
    "glossary"		, &uname_xhv_ns_uri_glossary	,
    "help"		, &uname_xhv_ns_uri_help	,
    "icon"		, &uname_xhv_ns_uri_icon	,
    "index"		, &uname_xhv_ns_uri_index	,
    "last"		, &uname_xhv_ns_uri_last	,
    "license"		, &uname_xhv_ns_uri_license	,
    "meta"		, &uname_xhv_ns_uri_meta	,
    "next"		, &uname_xhv_ns_uri_next	,
    "p3pv1"		, &uname_xhv_ns_uri_p3pv1	,
    "prev"		, &uname_xhv_ns_uri_prev	,
    "role"		, &uname_xhv_ns_uri_role	,
    "section"		, &uname_xhv_ns_uri_section	,
    "start"		, &uname_xhv_ns_uri_start	,
    "stylesheet"	, &uname_xhv_ns_uri_stylesheet	,
    "subsection"	, &uname_xhv_ns_uri_subsection	,
    "top"		, &uname_xhv_ns_uri_start	, /* NOT uname_xhv_ns_uri_top, because "top" is synonym for "start", see sect. 9.3. of "RDFa in XHTML:Syntax and Processing" */
    "up"		, &uname_xhv_ns_uri_up		};
  int pos = ecm_find_name (val, vals, sizeof (vals)/(2 * sizeof(void *)), 2 * sizeof(void *));
  if (ECM_MEM_NOT_FOUND == pos)
    return NULL;
  return ((caddr_t **)vals) [2 * pos + 1][0];
}

#define RDFA_ATTRSYNTAX_URI			0x01
#define RDFA_ATTRSYNTAX_SAFECURIE		0x02
#define RDFA_ATTRSYNTAX_CURIE			0x04
#define RDFA_ATTRSYNTAX_REL_REV_RESERVED	0x08
#define RDFA_ATTRSYNTAX_WS_LIST			0x10
#define RDFA_ATTRSYNTAX_EMPTY_ACCEPTABLE	0x20
#define RDFA_ATTRSYNTAX_EMPTY_MEANS_XSD_STRING	0x40

caddr_t
xp_rdfa_parse_attr_value (xparse_ctx_t *xp, xp_node_t * xn, char *attrname, char *attrvalue, int allowed_syntax, caddr_t **values_ret, int *values_count_ret)
{
  char *tail = attrvalue;
  char *token_start, *token_end;
  int curie_is_safe;
  char *curie_colon;
  caddr_t /*base = NULL,*/ expanded_token = NULL;
  int values_count, expanded_token_not_saved = 0;
#define free_unsaved_token() do { \
  if (expanded_token_not_saved) { \
      dk_free_box (expanded_token); \
      expanded_token = NULL; \
      expanded_token_not_saved = 0; } \
  } while (0)
#ifdef RDFXML_DEBUG
  if (((NULL != values_ret) ? 1 : 0) != ((NULL != values_count_ret) ? 1 : 0))
    GPF_T1 ("xp_" "rdfa_parse_attr_value(): bad call (1)");
  if (((NULL != values_ret) ? 1 : 0) != ((RDFA_ATTRSYNTAX_WS_LIST & allowed_syntax) ? 1 : 0))
    GPF_T1 ("xp_" "rdfa_parse_attr_value(): bad call (2)");
#endif
  if (NULL != values_ret)
    {
      if (NULL == values_ret[0])
        values_ret[0] = dk_alloc_box_zero (sizeof (caddr_t), DV_ARRAY_OF_POINTER);
      values_count = values_count_ret[0];
    }
  else
    values_count = 0;

next_token:
  if (RDFA_ATTRSYNTAX_WS_LIST & allowed_syntax)
    while (('\0' != tail[0]) && isspace (tail[0])) tail++;
  else if (isspace (tail[0]))
    {
      free_unsaved_token();
      xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100, "Whitespaces are not allowed for attribute %.20s", attrname);
    }
  if ('\0' == tail[0])
    {
      if (0 == values_count)
        {
          if (RDFA_ATTRSYNTAX_WS_LIST & allowed_syntax)
            return NULL;
          if (!((RDFA_ATTRSYNTAX_EMPTY_ACCEPTABLE | RDFA_ATTRSYNTAX_EMPTY_MEANS_XSD_STRING) & allowed_syntax))
            xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100, "Empty value is not allowed for attribute %.20s", attrname);
          expanded_token = (RDFA_ATTRSYNTAX_EMPTY_MEANS_XSD_STRING & allowed_syntax) ? uname_xmlschema_ns_uri_hash_string : uname___empty;
          if (NULL != values_ret)
            { /* I expect that zero length buffer is never passed */
              if (NULL != values_ret[0][values_count]) /* There's some old garbage to delete */
                dk_free_tree (values_ret[0][values_count]);
              values_ret[0][values_count++] = expanded_token;
              expanded_token_not_saved = 0;
              values_count_ret[0] = values_count;
            }
        }
      if (NULL != values_count_ret)
        values_count_ret[0] = values_count;
      return expanded_token;
    }
  if ((1 == values_count) && !(RDFA_ATTRSYNTAX_WS_LIST & allowed_syntax))
    {
      free_unsaved_token();
      xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100, "Multiple values are not allowed for attribute %.20s", attrname);
      if (NULL != values_count_ret)
        values_count_ret[0] = values_count;
      return expanded_token;
    }
  curie_is_safe = 0;
  if ('[' == tail[0])
    {
      if (!(RDFA_ATTRSYNTAX_SAFECURIE & allowed_syntax))
        {
          free_unsaved_token();
          xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100, "\"Safe CURIE\" syntax is not allowed for attribute \"%.20s\", ignored", attrname);
        }
      curie_is_safe = 1;
      tail++;
    }
  else
    curie_is_safe = 0;
  token_start = tail;
  curie_colon = NULL;
  if ((RDFA_ATTRSYNTAX_CURIE & allowed_syntax) || (curie_is_safe && (RDFA_ATTRSYNTAX_SAFECURIE & allowed_syntax)))
    {
      while (('\0' != tail[0]) && ('[' != tail[0]) && (']' != tail[0]) && (':' != tail[0]) && !isspace(tail[0])) tail++;
      if (':' == tail[0])
        {
          curie_colon = tail;
          tail++;
          while (('\0' != tail[0]) && (']' != tail[0]) && !isspace(tail[0])) tail++;
        }
      else
        { /* A CURIE without colon should be silently skipped, with two exceptions */
          if (curie_is_safe && (']' == tail[0]))
            {
              if (tail != token_start)
                {
                   tail++;
                   goto next_token; /* see above */
                }
            }
          else if (!curie_is_safe && !(RDFA_ATTRSYNTAX_REL_REV_RESERVED & allowed_syntax) && (('\0' == tail[0]) || isspace(tail[0])))
            goto next_token; /* see above */
        }
    }
  else
    {
      while (('\0' != tail[0]) && ('[' != tail[0]) && (']' != tail[0]) && !isspace(tail[0])) tail++;
    }
  token_end = tail;
  switch (tail[0])
    {
    case '[':
      free_unsaved_token();
      xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100,
        (curie_is_safe ?
          "Unterminated \"safe CURIE\" before '[' in the value of attribute \"%.20s\"" :
          "Character '[' is not allowed inside token in the value of attribute \"%.20s\"" ),
        attrname );
      break;
    case ']':
      if (curie_is_safe)
        tail++;
      else
        {
          free_unsaved_token();
          xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100,
            "Unexpected character ']' in the value of attribute \"%.20s\"",
            attrname );
        }
      break;
    default:
      if (curie_is_safe)
        {
          free_unsaved_token();
          xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100,
            "No closing ']' found at the end of \"safe CURIE\" in the value of attribute \"%.20s\"",
            attrname );
        }
      break;
    }
  if (NULL != values_ret)
    {
      if (values_count == BOX_ELEMENTS (values_ret[0]))
        {
          caddr_t new_buf = dk_alloc_box_zero (sizeof (caddr_t) * values_count * 2, DV_ARRAY_OF_POINTER);
          memcpy (new_buf, values_ret[0], box_length (values_ret[0]));
          dk_free_box (values_ret[0]);
          values_ret[0] = (caddr_t *)new_buf;
        }
      else if (NULL != values_ret[0][values_count]) /* There's some old garbage to delete */
        {
#ifdef RDFXML_DEBUG
          GPF_T1 ("xp_" "rdfa_parse_attr_value(): garbage?");
#endif
          dk_free_tree (values_ret[0][values_count]);
          values_ret[0][values_count] = NULL;
        }
    }
  /*base = xn->xn_xp->xp_rdfa_locals->xrdfal_base;*/
  if ((NULL != curie_colon) /*|| ((NULL != base) && ('\0' != base[0]))*/)
    {
      char saved_token_delim = token_end[0];
      token_end[0] = '\0';
      if (('_' == token_start[0]) && (curie_colon == token_start + 1))
        expanded_token = tf_bnode_iid (xp->xp_tf, box_dv_short_nchars (token_start+2, token_end-(token_start+2)));
      else if (curie_colon == token_start)
        { /* Note that the default prefix mapping may differ from usage to usage, it is xhtml vocab namespace only for RDFa */
          expanded_token = box_dv_short_strconcat (uname_xhv_ns_uri, curie_colon+1);
        }
      else
        expanded_token = xp_rdfa_expand_name (xn, token_start, curie_colon, 1/*, base*/);
      token_end[0] = saved_token_delim;
      if (NULL == expanded_token)
        xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100,
          "Bad token in the value of attribute \"%.20s\" (undeclared namespace?)",
          attrname );
    }
  else if (RDFA_ATTRSYNTAX_REL_REV_RESERVED & allowed_syntax)
    {
      char saved_token_delim = token_end[0];
      token_end[0] = '\0';
      expanded_token = rdfa_rel_rev_value_is_reserved (token_start);
      token_end[0] = saved_token_delim;
      if (NULL == expanded_token)
#if 1
        goto next_token; /* see above */
#else
        expanded_token = box_dv_short_nchars (token_start, token_end-token_start);
#endif
    }
  else if (curie_is_safe && (token_end == token_start))
    {
      xp_rdfa_locals_t *ancestor = xp->xp_rdfa_locals;
      while ((NULL != ancestor) && (NULL == ancestor->xrdfal_subj))
        ancestor = ancestor->xrdfal_parent;
      expanded_token = box_copy ((NULL != ancestor->xrdfal_subj) ? ancestor->xrdfal_subj : uname___empty);
    }
  else
    expanded_token = box_dv_short_nchars (token_start, token_end-token_start);
  expanded_token_not_saved = 1;
  if (NULL != values_ret)
    {
      values_ret[0][values_count] = expanded_token;
      expanded_token_not_saved = 0;
    }
  values_count++;
  goto next_token; /* see above */
}

void
xp_pop_rdfa_locals (xparse_ctx_t *xp)
{
  xp_rdfa_locals_t *inner = xp->xp_rdfa_locals;
  xp_rdfa_locals_t *parent = inner->xrdfal_parent;
  if ((NULL != inner->xrdfal_base) &&
    ((NULL == parent) || (inner->xrdfal_base != parent->xrdfal_base)) )
    dk_free_tree (inner->xrdfal_base);
  if ((NULL != inner->xrdfal_language) &&
    ((NULL == parent) || (inner->xrdfal_language != parent->xrdfal_language)) )
    dk_free_tree (inner->xrdfal_language);
  if ((NULL != inner->xrdfal_subj) &&
    ((NULL == parent) || ((inner->xrdfal_subj != parent->xrdfal_subj) && (inner->xrdfal_subj != parent->xrdfal_obj_res))) )
    dk_free_tree (inner->xrdfal_subj);
  if ((NULL != inner->xrdfal_obj_res) && (inner->xrdfal_obj_res != inner->xrdfal_subj) &&
    ((NULL == parent) || (inner->xrdfal_obj_res != parent->xrdfal_obj_res)) )
    dk_free_tree (inner->xrdfal_obj_res);
  if (NULL != inner->xrdfal_datatype)
    dk_free_tree (inner->xrdfal_datatype);
#ifdef RDFXML_DEBUG
  if (NULL != inner->xrdfal_ict_buffer)
    {
      int ofs;
      for (ofs = BOX_ELEMENTS (inner->xrdfal_ict_buffer); ofs--; /* no step */)
        {
          if (NULL != ((caddr_t *)(inner->xrdfal_ict_buffer))[ofs])
            GPF_T1 ("xp_" "pop_rdfa_locals(): lost data");
        }
    }
#endif
  xp->xp_rdfa_locals = parent;
  inner->xrdfal_parent = xp->xp_rdfa_free_list;
  xp->xp_rdfa_free_list = inner;
}


xp_rdfa_locals_t *
xp_push_rdfa_locals (xparse_ctx_t *xp)
{
  xp_rdfa_locals_t *outer = xp->xp_rdfa_locals;
  xp_rdfa_locals_t *inner;
  rdfa_ict_t *reused_buf;
  if (NULL != xp->xp_rdfa_free_list)
    {
      inner = xp->xp_rdfa_free_list;
      reused_buf = inner->xrdfal_ict_buffer;
      xp->xp_rdfa_free_list = inner->xrdfal_parent;
    }
  else
    {
      inner = dk_alloc (sizeof (xp_rdfa_locals_t));
      reused_buf = NULL;
    }
  memset (inner, 0, sizeof (xp_rdfa_locals_t));
  inner->xrdfal_ict_buffer = reused_buf;
  inner->xrdfal_parent = outer;
  xp->xp_rdfa_locals = inner;
  return inner;
}

void
xp_rdfa_set_base (xparse_ctx_t *xp, xp_rdfa_locals_t *inner, caddr_t new_base)
{
  xp_rdfa_locals_t *ancestor;
  caddr_t old_base_inside = NULL;
  for (ancestor = inner; (NULL != ancestor) && (RDFA_IN_HTML & ancestor->xrdfal_place_bits); ancestor = ancestor->xrdfal_parent)
    {
      caddr_t old_base = ancestor->xrdfal_base;
      if (old_base_inside != old_base)
        dk_free_box (old_base_inside);
      ancestor->xrdfal_base = new_base;
      old_base_inside = old_base;
    }
}

#define RDFA_ICT_FEED_OK	300
#define RDFA_ICT_IN_HEAD	301
#define RDFA_ICT_NO_OBJ		302
#define RDFA_ICT_INTERNAL_ERR	303

int
rdfa_ict_feed_or_leave (xparse_ctx_t *xp, xp_rdfa_locals_t *xrdfal, int ctr)
{
  rdfa_ict_t *ict;
  int last;
  static caddr_t stub_null = NULL;
  if (xrdfal->xrdfal_place_bits & RDFA_IN_HEAD)
    return RDFA_ICT_IN_HEAD;
  ict = xrdfal->xrdfal_ict_buffer + ctr;
  if (NULL == ict->ict_left)
#ifdef RDFXML_DEBUG
    GPF_T1("rdfa_" "ict_feed_or_leave(): NULL ict->ict_left");
#else
    return RDFA_ICT_INTERNAL_ERR;
#endif
  if (NULL == ict->ict_pred)
#ifdef RDFXML_DEBUG
    GPF_T1("rdfa_" "ict_feed_or_leave(): NULL ict->ict_pred");
#else
    return RDFA_ICT_INTERNAL_ERR;
#endif
  if (NULL == ict->ict_right)
    return RDFA_ICT_NO_OBJ;
  ict = xrdfal->xrdfal_ict_buffer + ctr;
  switch (ict->ict_pred_type)
    {
    case RDFA_ICT_PRED_REL_OR_TYPEOF:
      tf_triple (xp->xp_tf, ict->ict_left, ict->ict_pred, ict->ict_right);
      break;
    case RDFA_ICT_PRED_REV:
      tf_triple (xp->xp_tf, ict->ict_right, ict->ict_pred, ict->ict_left);
      break;
    case RDFA_ICT_PRED_PROPERTY:
      if (NULL == stub_null)
        stub_null = NEW_DB_NULL;
      tf_triple_l (xp->xp_tf, ict->ict_left, ict->ict_pred, ict->ict_right,
        (((NULL != ict->ict_datatype) && (uname___empty != ict->ict_datatype)) ? ict->ict_datatype : stub_null),
        ((NULL != ict->ict_language) ? ict->ict_language : stub_null) );
      break;
#ifdef RDFXML_DEBUG
    default:
      GPF_T1("rdfa_" "ict_feed_or_leave(): bad ict->ict_pred_type");
#endif
    }
  dk_free_box (ict->ict_left);
  dk_free_box (ict->ict_pred);
  dk_free_box (ict->ict_right);
  dk_free_box (ict->ict_datatype);
  dk_free_box (ict->ict_language);
  last = --(xrdfal->xrdfal_ict_count);
  if (last > ctr)
    memcpy (ict, xrdfal->xrdfal_ict_buffer + last, sizeof (rdfa_ict_t));
  memset (xrdfal->xrdfal_ict_buffer + last, 0, sizeof (rdfa_ict_t));
  return RDFA_ICT_FEED_OK;
}

void
rdfa_feed_or_make_ict (xparse_ctx_t *xp, xp_rdfa_locals_t *xrdfal, caddr_t left, caddr_t pred, caddr_t right, int pred_type, caddr_t dt, caddr_t lang)
{
  int ict_is_needed = 0;
  static caddr_t stub_null = NULL;
  if ((xrdfal->xrdfal_place_bits & RDFA_IN_BODY) || !(xrdfal->xrdfal_place_bits & RDFA_IN_HTML))
    {
      xp_expand_relative_uri (xrdfal->xrdfal_base, &left);
      xp_expand_relative_uri (xrdfal->xrdfal_base, &pred);
      if (RDFA_ICT_PRED_PROPERTY == pred_type)
        {
          if (uname___empty != dt)
            xp_expand_relative_uri (xrdfal->xrdfal_base, &dt);
        }
      else
        xp_expand_relative_uri (xrdfal->xrdfal_base, &right);
    }
  else
    ict_is_needed = 1;
#ifdef RDFXML_DEBUG
  if (NULL == left)
    GPF_T1("rdfa_" "feed_or_make_ict(): NULL left");
  if (NULL == pred)
    GPF_T1("rdfa_" "feed_or_make_ict(): NULL pred");
#endif
  if (NULL == right)
    ict_is_needed = 1;
  if (ict_is_needed)
    {
      rdfa_ict_t *ict;
      int buf_in_use = xrdfal->xrdfal_ict_count * sizeof (rdfa_ict_t);
      if (NULL == xrdfal->xrdfal_ict_buffer)
        xrdfal->xrdfal_ict_buffer = dk_alloc_box_zero (sizeof (rdfa_ict_t), DV_ARRAY_OF_POINTER);
      if (box_length (xrdfal->xrdfal_ict_buffer) <= buf_in_use)
        {
          rdfa_ict_t *new_buf;
#ifdef RDFXML_DEBUG
          if (box_length (xrdfal->xrdfal_ict_buffer) < buf_in_use)
            GPF_T1("rdfa_" "feed_or_make_ict(): corrupted buffer allocation");
#endif
          new_buf = (rdfa_ict_t *)dk_alloc_box_zero (buf_in_use * 2, DV_ARRAY_OF_POINTER);
          memcpy (new_buf, xrdfal->xrdfal_ict_buffer, buf_in_use);
          dk_free_box ((caddr_t)(xrdfal->xrdfal_ict_buffer));
          xrdfal->xrdfal_ict_buffer = new_buf;
        }
      ict = xrdfal->xrdfal_ict_buffer + (xrdfal->xrdfal_ict_count)++;
      ict->ict_left = left;
      ict->ict_pred = pred;
      ict->ict_right = right;
      ict->ict_pred_type = pred_type;
      ict->ict_datatype = dt;
      ict->ict_language = lang;
    }
  else
    {
      switch (pred_type)
        {
        case RDFA_ICT_PRED_REL_OR_TYPEOF:
          tf_triple (xp->xp_tf, left, pred, right);
          break;
        case RDFA_ICT_PRED_REV:
          tf_triple (xp->xp_tf, right, pred, left);
          break;
        case RDFA_ICT_PRED_PROPERTY:
          if (NULL == stub_null)
            stub_null = NEW_DB_NULL;
          tf_triple_l (xp->xp_tf, left, pred, right,
            (((NULL != dt) && (uname___empty != dt)) ? dt : stub_null),
            ((NULL != lang) ? lang : stub_null) );
          break;
#ifdef RDFXML_DEBUG
        default:
          GPF_T1("rdfa_" "ict_feed_or_leave(): bad ict->ict_pred_type");
#endif
        }
      dk_free_box (left);
      dk_free_box (pred);
      dk_free_box (right);
      dk_free_box (dt);
      dk_free_box (lang);
    }
}

void
xp_rdfa_element (void *userdata, char * name, vxml_parser_attrdata_t *attrdata)
{
  xparse_ctx_t * xp = (xparse_ctx_t*) userdata;
  xp_rdfa_locals_t *outer = xp->xp_rdfa_locals;
  xp_rdfa_locals_t *inner = NULL; /* This is not allocated at all if there's nothing "interesting" in the tag */
  xp_tmp_t *xpt = xp->xp_tmp;
  xp_node_t *xn = xp->xp_current;
  int inx, fill, n_attrs, n_ns, xn_is_allocated = 0;
  char *local_name;
  int rel_rev_attrcount = 0, rel_pred_count = 0, rev_pred_count = 0, prop_pred_count = 0, typeof_count = 0;
  int src_prio = 0xff; /* 1 for "about", 2 for "src" */
  int href_prio = 0xff; /* 3 for "resource", 4 for "href" */
  int outer_place_bits = outer->xrdfal_place_bits;
  int inner_place_bits = outer_place_bits; /* Place bits are first inherited, then changed (OR-ed) */
  int need_rdfa_local = 0, parent_obj_should_be_set = 0;
  int ctr;
  caddr_t subj;
#ifdef RDFXML_DEBUG
  if (xpt->xpt_base || xpt->xpt_dt || xpt->xpt_lang || xpt->xpt_obj_content || xpt->xpt_obj_res || xpt->xpt_src || xpt->xpt_href)
    GPF_T1("xp_" "rdfa_element(): nonempty xpt");
#endif
#ifdef RECOVER_RDF_VALUE
  caddr_t rdf_val = NULL;
#endif
  if (RDFA_IN_LITERAL & outer_place_bits)
    {
      if (!((RDFA_IN_STRLITERAL | RDFA_IN_XMLLITERAL) & outer_place_bits))
        {
          outer->xrdfal_place_bits |= RDFA_IN_XMLLITERAL;
          outer_place_bits = outer->xrdfal_place_bits;
          outer->xrdfal_datatype = uname_rdf_ns_uri_XMLLiteral;
        }
      if ((RDFA_IN_UNUSED | RDFA_IN_STRLITERAL) & outer_place_bits)
        outer->xrdfal_boring_opened_elts++;
      else
        xp_element (userdata, name, attrdata);
      return;
    }
/* Let's make xp->xp_free_list nonempty just to not duplicate this code in few places below */
  if (NULL == xp->xp_free_list)
    {
      xp->xp_free_list = dk_alloc (sizeof (xp_node_t));
      xp->xp_free_list->xn_parent = NULL;
    }
  n_ns = attrdata->local_nsdecls_count;
  if (n_ns)
    {
      caddr_t *save_ns;
      xn = xp->xp_free_list;
      xp->xp_free_list = xn->xn_parent;
      memset (xn, 0, sizeof (xp_node_t));
      xn->xn_xp = xp;
      xn->xn_parent = xp->xp_current;
      xp->xp_current = xn;
      xn_is_allocated = 1;
      need_rdfa_local = 1;
      save_ns = (caddr_t*) dk_alloc_box (2 * n_ns * sizeof (caddr_t), DV_ARRAY_OF_POINTER);
      /* Trick here: xn->xn_attrs is set to xn->xn_namespaces in order to free memory on errors or element end. */
      xn->xn_attrs = xn->xn_namespaces = save_ns;
      fill = 0;
      for (inx = 0; inx < n_ns; inx++)
        {
          save_ns[fill++] = box_dv_uname_string (attrdata->local_nsdecls[inx].nsd_prefix);
          save_ns[fill++] = box_dv_uname_string (attrdata->local_nsdecls[inx].nsd_uri);
        }
    }
/* Setting place bits */
  local_name = strchr (name, ':');
  if (NULL == local_name)
    local_name = name;
  if (RDFA_IN_HEAD & outer_place_bits)
    {
      if (!strcmp (local_name, "base"))
        {
          if (RDFA_IN_BASE & outer_place_bits)
            xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100, "Element \"base\" can not appear inside other \"base\" elelent");
          inner_place_bits |= RDFA_IN_BASE;
          need_rdfa_local = 1;
        }
    }
  if (RDFA_IN_HTML & outer_place_bits)
    {
      if (!strcmp (local_name, "head"))
        {
          if ((RDFA_IN_HEAD | RDFA_IN_BODY) & outer_place_bits)
            xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100, "Element \"head\" can not appear inside %s elelent", (RDFA_IN_HEAD & outer_place_bits) ? "other \"head\"" : "\"body\"");
          else
            inner_place_bits |= RDFA_IN_HEAD;
          need_rdfa_local = 1;
        }
      else if (!strcmp (local_name, "body"))
        {
          if ((RDFA_IN_HEAD | RDFA_IN_BODY) & outer_place_bits)
            xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100, "Element \"body\" can not appear inside %s elelent", (RDFA_IN_BODY & outer_place_bits) ? "other \"body\"" : "\"head\"");
          else
            inner_place_bits |= RDFA_IN_BODY;
          need_rdfa_local = 1;
        }
    }
  if (!strcmp (local_name, "html"))
    {
      if (RDFA_IN_HTML & outer_place_bits)
        xmlparser_logprintf (xp->xp_parser, XCFG_ERROR, 100, "Element \"html\" can not appear inside other \"html\" element");
      else
        inner_place_bits |= RDFA_IN_HTML;
      need_rdfa_local = 1;
    }
  n_attrs = attrdata->local_attrs_count;
  for (inx = 0; inx < n_attrs; inx ++)
    {
      char *raw_aname = attrdata->local_attrs[inx].ta_raw_name.lm_memblock;
      caddr_t avalue;
      int acode = rdfa_attribute_code (raw_aname);
      if (!acode)
        continue;
      need_rdfa_local = 1;
      avalue = attrdata->local_attrs[inx].ta_value;
      switch (acode)
        {
        case RDFA_ATTR_ABOUT:
          if (1 <= src_prio)
            {
              dk_free_tree (xpt->xpt_src);
              xpt->xpt_src = NULL; /* to avoid second delete of freed value in case of error inside xp_rdfa_parse_attr_value() */
              xpt->xpt_src = xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
                RDFA_ATTRSYNTAX_URI | RDFA_ATTRSYNTAX_SAFECURIE | RDFA_ATTRSYNTAX_EMPTY_ACCEPTABLE,
                NULL, NULL );
              src_prio = 1;
            }
          break;
        case RDFA_ATTR_CONTENT:
          xpt->xpt_obj_content = box_dv_short_string (avalue);
          break;
        case RDFA_ATTR_DATATYPE:
          xpt->xpt_dt = xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
            RDFA_ATTRSYNTAX_CURIE | RDFA_ATTRSYNTAX_EMPTY_ACCEPTABLE,
            NULL, NULL );
          break;
        case RDFA_ATTR_HREF:
          if (RDFA_IN_BASE & inner_place_bits)
            {
              dk_free_tree (xpt->xpt_href);
              xpt->xpt_href = NULL; /* to avoid second delete of freed value in case of error inside xp_rdfa_parse_attr_value() */
              xpt->xpt_href = xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
                RDFA_ATTRSYNTAX_URI | RDFA_ATTRSYNTAX_EMPTY_ACCEPTABLE,
                NULL, NULL );
              xp_rdfa_set_base (xp, outer, xpt->xpt_href);
              xpt->xpt_href = NULL;
              inner_place_bits |= RDFA_IN_UNUSED;
            }
          else if (4 <= href_prio)
            {
              dk_free_tree (xpt->xpt_href);
              xpt->xpt_href = NULL; /* to avoid second delete of freed value in case of error inside xp_rdfa_parse_attr_value() */
              xpt->xpt_href = xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
                RDFA_ATTRSYNTAX_URI | RDFA_ATTRSYNTAX_EMPTY_ACCEPTABLE,
                NULL, NULL );
              href_prio = 4;
            }
          break;
        case RDFA_ATTR_PROPERTY:
          xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
            RDFA_ATTRSYNTAX_CURIE | RDFA_ATTRSYNTAX_WS_LIST,
            &(xpt->xpt_prop_preds), &prop_pred_count );
          break;
        case RDFA_ATTR_REL:
          xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
            RDFA_ATTRSYNTAX_CURIE | RDFA_ATTRSYNTAX_REL_REV_RESERVED | RDFA_ATTRSYNTAX_WS_LIST,
            &(xpt->xpt_rel_preds), &rel_pred_count );
          rel_rev_attrcount++;
          break;
        case RDFA_ATTR_RESOURCE:
          if (3 <= href_prio)
            {
              dk_free_tree (xpt->xpt_href);
              xpt->xpt_href = NULL; /* to avoid second delete of freed value in case of error inside xp_rdfa_parse_attr_value() */
              xpt->xpt_href = xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
                RDFA_ATTRSYNTAX_URI | RDFA_ATTRSYNTAX_SAFECURIE | RDFA_ATTRSYNTAX_EMPTY_ACCEPTABLE,
                NULL, NULL );
              href_prio = 3;
            }
          break;
        case RDFA_ATTR_REV:
          xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
            RDFA_ATTRSYNTAX_CURIE | RDFA_ATTRSYNTAX_REL_REV_RESERVED | RDFA_ATTRSYNTAX_WS_LIST,
            &(xpt->xpt_rev_preds), &rev_pred_count );
          rel_rev_attrcount++;
          break;
        case RDFA_ATTR_SRC:
          if (2 <= src_prio)
            {
              dk_free_tree (xpt->xpt_src);
              xpt->xpt_src = NULL; /* to avoid second delete of freed value in case of error inside xp_rdfa_parse_attr_value() */
              xpt->xpt_src = xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
                RDFA_ATTRSYNTAX_URI | RDFA_ATTRSYNTAX_EMPTY_ACCEPTABLE,
                NULL, NULL );
              src_prio = 2;
            }
          break;
        case RDFA_ATTR_TYPEOF:
          xp_rdfa_parse_attr_value (xp, xn, raw_aname, avalue,
            RDFA_ATTRSYNTAX_CURIE | RDFA_ATTRSYNTAX_WS_LIST,
            &(xpt->xpt_typeofs), &typeof_count );
          break;
        case RDFA_ATTR_XML_BASE:
          if (RDFA_IN_HTML & inner_place_bits)
            break;
          if (NULL != xpt->xpt_base)
            dk_free_tree (xpt->xpt_base);
          xpt->xpt_base = box_dv_short_string (avalue);
          break;
        case RDFA_ATTR_XML_LANG:
          if (NULL != xpt->xpt_lang)
            dk_free_tree (xpt->xpt_lang);
          xpt->xpt_lang = box_dv_short_string (avalue);
          break;
#ifdef RDFXML_DEBUG
        default: GPF_T;
#endif
        }
    }
/* At this point, all attributes are retrieved. The rest is straightforward implementation of "Processing model" */
/* Setting the [new subject] */
  for (;;)
    {
      subj = xpt->xpt_src;
      if (NULL != subj)
        break;
      if (!rel_rev_attrcount)
        {
          subj = xpt->xpt_href;
          if (NULL != subj)
            break;
        }
      if ((RDFA_IN_HTML & outer_place_bits) &&
        !((RDFA_IN_HEAD | RDFA_IN_BODY) & outer_place_bits) &&
        ((RDFA_IN_HEAD | RDFA_IN_BODY) & inner_place_bits) )
        subj = uname___empty;
      else if (0 != typeof_count)
        subj = tf_bnode_iid (xp->xp_tf, NULL);
      break;
    }
  if (prop_pred_count)
    {
      inner_place_bits |= RDFA_IN_LITERAL;
      if (NULL != xpt->xpt_obj_content)
        inner_place_bits |= RDFA_IN_UNUSED;
      else if (NULL != xpt->xpt_dt)
        {
          if (strcmp (xpt->xpt_dt, uname_rdf_ns_uri_XMLLiteral))
            inner_place_bits |= RDFA_IN_STRLITERAL;
          else
            inner_place_bits |= RDFA_IN_XMLLITERAL;
        }
    }
/* Escape if nothing interesting is detected at all */
  if (!need_rdfa_local)
    {
      outer->xrdfal_boring_opened_elts++;
      return;
    }
/* There is something interesting so the stack should grow */
  if (!xn_is_allocated)
    {
      xn = xp->xp_free_list;
      xp->xp_free_list = xn->xn_parent;
      memset (xn, 0, sizeof (xp_node_t));
      xn->xn_xp = xp;
      xn->xn_parent = xp->xp_current;
      xp->xp_current = xn;
    }
  inner = xp_push_rdfa_locals (xp);
#ifdef DEBUG
  if (NULL != xp->xp_boxed_name)
    GPF_T1("Memory leak in xp->xp_boxed_name");
#endif
  inner->xrdfal_xn = xn;
  inner->xrdfal_place_bits = inner_place_bits;
  if (NULL != xpt->xpt_base)
    {
      inner->xrdfal_base = xpt->xpt_base;
      xpt->xpt_base = NULL;
    }
  else
    inner->xrdfal_base = outer->xrdfal_base;
  if (NULL != subj)
    {
      inner->xrdfal_subj = subj;
      if (subj == xpt->xpt_src)
        xpt->xpt_src = NULL;
      else if (subj == xpt->xpt_href)
        {
          if (rel_rev_attrcount)
            xpt->xpt_href = box_copy (xpt->xpt_href);
          else
            xpt->xpt_href = NULL;
        }
    }
  else if ((NULL != outer) && (NULL != outer->xrdfal_obj_res))
    inner->xrdfal_subj = outer->xrdfal_obj_res;
  else if (rel_rev_attrcount || prop_pred_count)
    {
      inner->xrdfal_subj = tf_bnode_iid (xp->xp_tf, NULL);
      parent_obj_should_be_set = 1;
    }
  else
    inner->xrdfal_subj = NULL;
  if (rel_rev_attrcount)
    {
      inner->xrdfal_obj_res = xpt->xpt_href;
      xpt->xpt_href = NULL;
    }
  else if ((!parent_obj_should_be_set) && (NULL == subj))
    inner->xrdfal_obj_res = outer->xrdfal_obj_res;
  inner->xrdfal_datatype = xpt->xpt_dt;
  xpt->xpt_dt = NULL;
  if (NULL != xpt->xpt_lang)
    {
      inner->xrdfal_language = xpt->xpt_lang;
      xpt->xpt_lang = NULL;
    }
  else
    inner->xrdfal_language = outer->xrdfal_language;
  inner->xrdfal_boring_opened_elts = 0;
#ifdef RDFXML_DEBUG
  if (inner->xrdfal_ict_count)
    GPF_T1("xp_" "rdfa_element(): ict buffer is not empty");
#endif
/* Finally we can make triples, starting from incomplete triples at upper levels */
  if ((NULL != inner->xrdfal_subj) && (inner->xrdfal_subj != outer->xrdfal_obj_res))
    {
      caddr_t old_outer_obj = outer->xrdfal_obj_res;
      xp_rdfa_locals_t * ancestor = outer;
      for (;;)
        {
          for (ctr = ancestor->xrdfal_ict_count; ctr--; /* no step */) /* The order is important */
            {
              rdfa_ict_t *ict = ancestor->xrdfal_ict_buffer + ctr;
              if ((RDFA_ICT_PRED_PROPERTY != ict->ict_pred_type) && (NULL == ict->ict_right))
                {
                  rdfa_feed_or_make_ict (xp, ancestor, box_copy (ict->ict_left), box_copy (ict->ict_pred), box_copy (inner->xrdfal_subj), ict->ict_pred_type, NULL, NULL);
                  ict->ict_used_as_template = 1;
                }
            }
          if (parent_obj_should_be_set)
            ancestor->xrdfal_obj_res = inner->xrdfal_subj;
          ancestor = ancestor->xrdfal_parent;
          if (NULL == ancestor)
            break;
          if ((old_outer_obj != ancestor->xrdfal_obj_res) || (outer->xrdfal_subj != ancestor->xrdfal_subj))
            break;
        }
    }
  if ((NULL != inner->xrdfal_subj) && typeof_count)
    {
      for (ctr = typeof_count; ctr--; /* no step */)
        {
          caddr_t type_uri = xpt->xpt_typeofs[ctr];
          xpt->xpt_typeofs[ctr] = NULL;
          rdfa_feed_or_make_ict (xp, inner, box_copy (inner->xrdfal_subj), uname_rdf_ns_uri_type, type_uri, RDFA_ICT_PRED_REL_OR_TYPEOF, NULL, NULL);
        }
    }
  for (ctr = rel_pred_count; ctr--; /* no step */)
    {
      caddr_t p = xpt->xpt_rel_preds[ctr];
      xpt->xpt_rel_preds[ctr] = NULL;
      rdfa_feed_or_make_ict (xp, inner, box_copy (inner->xrdfal_subj), p, box_copy (inner->xrdfal_obj_res), RDFA_ICT_PRED_REL_OR_TYPEOF, NULL, NULL);
    }
  for (ctr = rev_pred_count; ctr--; /* no step */)
    {
      caddr_t p = xpt->xpt_rev_preds[ctr];
      xpt->xpt_rev_preds[ctr] = NULL;
      rdfa_feed_or_make_ict (xp, inner, box_copy (inner->xrdfal_subj), p, box_copy (inner->xrdfal_obj_res), RDFA_ICT_PRED_REV, NULL, NULL);
    }
  for (ctr = prop_pred_count; ctr--; /* no step */)
    {
      caddr_t p = xpt->xpt_prop_preds[ctr];
      caddr_t val = xpt->xpt_obj_content;
      caddr_t dt = inner->xrdfal_datatype;
      caddr_t lang = (NULL == dt) ? box_copy (inner->xrdfal_language) : NULL;
      xpt->xpt_prop_preds[ctr] = NULL;
      if (0 < ctr)
        {
          val = box_copy (val);
          dt = box_copy (dt);
        }
      else
        {
          xpt->xpt_obj_content = NULL;
          inner->xrdfal_datatype = NULL;
        }
      rdfa_feed_or_make_ict (xp, inner, box_copy (inner->xrdfal_subj), p, val, RDFA_ICT_PRED_PROPERTY, dt, lang);
    }
  if (!prop_pred_count)
    {
      dk_free_box (xpt->xpt_obj_content); xpt->xpt_obj_content = NULL;
      dk_free_box (inner->xrdfal_datatype); inner->xrdfal_datatype = NULL;
    }
  if ((NULL == inner->xrdfal_obj_res) && !rel_rev_attrcount)
    inner->xrdfal_obj_res = inner->xrdfal_subj;
}

void
xp_rdfa_element_end (void *userdata, const char * name)
{
  xparse_ctx_t *xp = (xparse_ctx_t*) userdata;
  xp_rdfa_locals_t *inner = xp->xp_rdfa_locals;
  xp_node_t *current_node, *parent_node;
  int inner_place_bits = inner->xrdfal_place_bits;
  int ctr;
  if (xp->xp_current != inner->xrdfal_xn)
    {
      if (!(RDFA_IN_XMLLITERAL & inner_place_bits))
        GPF_T1 ("xp_" "rdfa_element_end(): misaligned stacks");
      xp_element_end (userdata, name);
      return;
    }
  if (inner->xrdfal_boring_opened_elts)
    {
      inner->xrdfal_boring_opened_elts--;
      return;
    }
  inner_place_bits = inner->xrdfal_place_bits;
  current_node = xp->xp_current;
  if ((RDFA_IN_BASE & inner_place_bits) && !(RDFA_IN_UNUSED & inner_place_bits))
    {
      caddr_t new_base = strses_string (xp->xp_strses);
      strses_flush (xp->xp_strses);
      xp_rdfa_set_base (xp, inner, new_base);
    }
  if (RDFA_IN_LITERAL & inner_place_bits)
    {
      caddr_t obj = NULL;
      int obj_use_count = 0;
      for (ctr = inner->xrdfal_ict_count; ctr--; /* no step */)
        {
          rdfa_ict_t *ict = inner->xrdfal_ict_buffer + ctr;
          if ((RDFA_ICT_PRED_PROPERTY != ict->ict_pred_type) || (NULL != ict->ict_right))
            continue;
          obj_use_count++;
        }
      if (RDFA_IN_XMLLITERAL & inner_place_bits)
        {
          dk_set_t children;
          caddr_t *literal_head;
          caddr_t literal_tree;
          XP_STRSES_FLUSH (xp);
          children = dk_set_nreverse (current_node->xn_children);
          literal_head = (caddr_t *)list (1, uname__root);
          children = CONS (literal_head, children);
          literal_tree = list_to_array (children);
          if (obj_use_count)
            {
              xml_tree_ent_t *literal_xte;
              literal_xte = xte_from_tree (literal_tree, xp->xp_qi);
              obj = (caddr_t) literal_xte;
            }
          else
            dk_free_tree (literal_tree);
        }
      else
        {
          if (obj_use_count)
            obj = strses_string (xp->xp_strses);
          strses_flush (xp->xp_strses);
        }
      for (ctr = inner->xrdfal_ict_count; ctr--; /* no step */)
        {
          rdfa_ict_t *ict = inner->xrdfal_ict_buffer + ctr;
          if ((RDFA_ICT_PRED_PROPERTY != ict->ict_pred_type) || (NULL != ict->ict_right))
            continue;
          if (RDFA_IN_XMLLITERAL & inner_place_bits)
            ict->ict_datatype = uname_rdf_ns_uri_XMLLiteral;
          ict->ict_right = --obj_use_count ? box_copy_tree (obj) : obj;
          rdfa_ict_feed_or_leave (xp, inner, ctr);
        }
#ifdef RDFXML_DEBUG
      if (obj_use_count)
        GPF_T1 ("xp_" "rdfa_element_end(): obj_use_count is out of sync");
#endif
    }
  if (RDFA_IN_HEAD & inner_place_bits)
    {
      xp_rdfa_locals_t *parent = inner->xrdfal_parent;
      int inner_size = sizeof (rdfa_ict_t) * inner->xrdfal_ict_count;
      int parent_size = sizeof (rdfa_ict_t) * parent->xrdfal_ict_count;
      int needed_size = inner_size + parent_size;
      if (NULL == parent->xrdfal_ict_buffer)
        {
          if (inner->xrdfal_ict_count)
            {
              parent->xrdfal_ict_buffer = inner->xrdfal_ict_buffer;
              inner->xrdfal_ict_buffer = NULL;
            }
        }
      else if (box_length (parent->xrdfal_ict_buffer) < needed_size)
        {
          rdfa_ict_t *new_buf = dk_alloc_box_zero (needed_size, DV_ARRAY_OF_POINTER);
          memcpy (new_buf, parent->xrdfal_ict_buffer, parent_size);
          memset (parent->xrdfal_ict_buffer, 0, parent_size);
          memcpy (new_buf + parent->xrdfal_ict_count, inner->xrdfal_ict_buffer, inner_size);
          memset (inner->xrdfal_ict_buffer, 0, inner_size);
          dk_free_tree (parent->xrdfal_ict_buffer);
          parent->xrdfal_ict_buffer = new_buf;
        }
      else
        {
          memcpy (parent->xrdfal_ict_buffer + parent->xrdfal_ict_count, inner->xrdfal_ict_buffer, inner_size);
          memset (inner->xrdfal_ict_buffer, 0, inner_size);
        }
      parent->xrdfal_ict_count += inner->xrdfal_ict_count;
      inner->xrdfal_ict_count = 0;
    }
  else
    {
      for (ctr = inner->xrdfal_ict_count; ctr--; /* no step */)
        {
          rdfa_ict_t *ict = inner->xrdfal_ict_buffer + ctr;
          xp_expand_relative_uri (inner->xrdfal_base, &(ict->ict_left));
          xp_expand_relative_uri (inner->xrdfal_base, &(ict->ict_pred));
          if (RDFA_ICT_PRED_PROPERTY == ict->ict_pred_type)
            {
              if (uname___empty != ict->ict_datatype)
                xp_expand_relative_uri (inner->xrdfal_base, &(ict->ict_datatype));
            }
          else
            xp_expand_relative_uri (inner->xrdfal_base, &(ict->ict_right));
          if (RDFA_ICT_NO_OBJ == rdfa_ict_feed_or_leave (xp, inner, ctr))
            {
              if (!ict->ict_used_as_template)
                xmlparser_logprintf (xp->xp_parser, XCFG_WARNING, 500,
                  (RDFA_ICT_PRED_REV == ict->ict_pred_type) ?
                    "Predicate %.200s with object %.200s has no subject" :
                    "Property %.200s of subject %.200s has no value",
                  (DV_IRI_ID == DV_TYPE_OF (ict->ict_left)) ? "(blank node)" : ict->ict_left,
                  (DV_IRI_ID == DV_TYPE_OF (ict->ict_pred)) ? "(blank node)" : ict->ict_pred );
              dk_free_box (ict->ict_left); ict->ict_left = NULL;
              dk_free_box (ict->ict_pred); ict->ict_pred = NULL;
              ict->ict_pred_type = 0;
              ict->ict_used_as_template = 0;
            }
        }
    }
  parent_node = xp->xp_current->xn_parent;
  dk_free_tree (current_node->xn_attrs);
  xp->xp_current = parent_node;
  current_node->xn_parent = xp->xp_free_list;
  xp->xp_free_list = current_node;
  xp_pop_rdfa_locals (xp);
}

void
xp_rdfa_id (void *userdata, char * name)
{
  xparse_ctx_t * xp = (xparse_ctx_t*) userdata;
  xp_rdfa_locals_t *inner = xp->xp_rdfa_locals;
  if (RDFA_IN_XMLLITERAL & inner->xrdfal_place_bits)
    xp_id (userdata, name);
}

void
xp_rdfa_character (void *userdata,  char * s, int len)
{
  xparse_ctx_t *xp = (xparse_ctx_t*) userdata;
  xp_rdfa_locals_t *inner = xp->xp_rdfa_locals;
  int inner_place_bits = inner->xrdfal_place_bits;
  if (((RDFA_IN_BASE & inner_place_bits) || (RDFA_IN_LITERAL & inner_place_bits)) &&
    !(RDFA_IN_UNUSED & inner_place_bits) )
    session_buffered_write (xp->xp_strses, s, len);
}

void
xp_rdfa_entity (void *userdata, const char * refname, int reflen, int isparam, const xml_def_4_entity_t *edef)
{
  xparse_ctx_t *xp = (xparse_ctx_t *) userdata;
  xp_rdfa_locals_t *inner = xp->xp_rdfa_locals;
  if (RDFA_IN_XMLLITERAL & inner->xrdfal_place_bits)
    xp_entity (userdata, refname, reflen, isparam, edef);
  else if (RDFA_IN_STRLITERAL & inner->xrdfal_place_bits)
    xmlparser_logprintf (xp->xp_parser, XCFG_FATAL, 100, "Entities are not supported in string literal object");
}

void
xp_rdfa_pi (void *userdata, const char *target, const char *data)
{
  xparse_ctx_t *xp = (xparse_ctx_t *) userdata;
  xp_rdfa_locals_t *inner = xp->xp_rdfa_locals;
  if (RDFA_IN_XMLLITERAL & inner->xrdfal_place_bits)
    xp_pi (userdata, target, data);
}

void
xp_rdfa_comment (void *userdata, const char *text)
{
  xparse_ctx_t *xp = (xparse_ctx_t *) userdata;
  xp_rdfa_locals_t *inner = xp->xp_rdfa_locals;
  if (RDFA_IN_XMLLITERAL & inner->xrdfal_place_bits)
    xp_comment (userdata, text);
}



/* Part 3. Common parser invocation routine */

void
rdfxml_parse (query_instance_t * qi, caddr_t text, caddr_t *err_ret,
  int mode_bits, const char *source_name, caddr_t base_uri, caddr_t graph_uri,
  ccaddr_t *cbk_names, caddr_t app_env,
  const char *enc, lang_handler_t *lh
   /*, caddr_t dtd_config, dtd_t **ret_dtd,
   id_hash_t **ret_id_cache, xml_ns_2dict_t *ret_ns_2dict*/ )
{
  int dtp_of_text = box_tag (text);
  vxml_parser_config_t config;
  vxml_parser_t * parser;
  xparse_ctx_t context;
  triple_feed_t *tf;
  int rc;
  xp_node_t *xn;
  xp_rdfxml_locals_t *root_xrl;
  xml_read_iter_env_t xrie;
  static caddr_t default_dtd_config = NULL;
  memset (&xrie, 0, sizeof (xml_read_iter_env_t));
  if (DV_BLOB_XPER_HANDLE == dtp_of_text)
    sqlr_new_error ("42000", "XM031", "Unable to parse RDF/XML from a persistent XML object");
  if (!xml_set_xml_read_iter (qi, text, &xrie, &enc))
    sqlr_new_error ("42000", "XM032",
      "Unable to parse RDF/XML from data of type %s (%d)", dv_type_title (dtp_of_text), dtp_of_text);
  xn = (xp_node_t *) dk_alloc (sizeof (xp_node_t));
  memset (xn, 0, sizeof(xp_node_t));
  memset (&context, 0, sizeof (context));
  context.xp_current = xn;
  xn->xn_xp = &context;
  root_xrl = (xp_rdfxml_locals_t *) dk_alloc (sizeof (xp_rdfxml_locals_t));
  memset (root_xrl, 0, sizeof (xp_rdfxml_locals_t));
  root_xrl->xrl_base = base_uri;
  root_xrl->xrl_parsetype = XRL_PARSETYPE_TOP_LEVEL;
  root_xrl->xrl_xn = xn;
  context.xp_strses = strses_allocate ();
  context.xp_top = xn;
  context.xp_rdfxml_locals = root_xrl;
  context.xp_qi = qi;
  memset (&config, 0, sizeof(config));
  config.input_is_wide = xrie.xrie_text_is_wide;
  config.input_is_ge = ((mode_bits & RDFXML_OMIT_TOP_RDF) ? GE_XML : 0);
  config.input_is_html = ((mode_bits >> 8) & 0xff);
  config.input_is_xslt = 0;
  config.user_encoding_handler = intl_find_user_charset;
  config.initial_src_enc_name = enc;
  config.uri_resolver = (VXmlUriResolver)(xml_uri_resolve_like_get);
  config.uri_reader = (VXmlUriReader)(xml_uri_get);
  config.uri_appdata = qi; /* Both xml_uri_resolve_like_get and xml_uri_get uses qi as first argument */
  config.error_reporter = (VXmlErrorReporter)(sqlr_error);
  config.uri = ((NULL == base_uri) ? uname___empty : base_uri);
  if (NULL == default_rdf_dtd_config)
    default_rdf_dtd_config = box_dv_short_string ("Validation=DISABLE SchemaDecl=DISABLE IdCache=DISABLE");
  config.dtd_config = default_dtd_config;
  config.root_lang_handler = lh;
  parser = VXmlParserCreate (&config);
  parser->fill_ns_2dict = 0;
  context.xp_parser = parser;
  VXmlSetUserData (parser, &context);
  if (mode_bits & RDFXML_IN_ATTRIBUTES)
    {
      xp_rdfa_locals_t *root_xrdfal = xp_push_rdfa_locals (&context);
      root_xrdfal->xrdfal_base = box_copy (base_uri);
      context.xp_tmp = dk_alloc_box_zero (sizeof (xp_tmp_t), DV_ARRAY_OF_POINTER);
      VXmlSetElementHandler (parser, (VXmlStartElementHandler) xp_rdfa_element, xp_rdfa_element_end);
      VXmlSetIdHandler (parser, (VXmlIdHandler)xp_rdfa_id);
      VXmlSetCharacterDataHandler (parser, (VXmlCharacterDataHandler) xp_rdfa_character);
      VXmlSetEntityRefHandler (parser, (VXmlEntityRefHandler) xp_rdfa_entity);
      VXmlSetProcessingInstructionHandler (parser, (VXmlProcessingInstructionHandler) xp_rdfa_pi);
      VXmlSetCommentHandler (parser, (VXmlCommentHandler) xp_rdfa_comment);
    }
  else
    {
      VXmlSetElementHandler (parser, (VXmlStartElementHandler) xp_rdfxml_element, xp_rdfxml_element_end);
      VXmlSetIdHandler (parser, (VXmlIdHandler)xp_rdfxml_id);
      VXmlSetCharacterDataHandler (parser, (VXmlCharacterDataHandler) xp_rdfxml_character);
      VXmlSetEntityRefHandler (parser, (VXmlEntityRefHandler) xp_rdfxml_entity);
      VXmlSetProcessingInstructionHandler (parser, (VXmlProcessingInstructionHandler) xp_rdfxml_pi);
      VXmlSetCommentHandler (parser, (VXmlCommentHandler) xp_rdfxml_comment);
    }
  if (NULL != xrie.xrie_iter)
    {
      rdfxml_dbg_printf(("\n\n rdfxml_parse() will parse text input"));
      VXmlParserInput (parser, xrie.xrie_iter, xrie.xrie_iter_data);
    }
  else
    {
      rdfxml_dbg_printf(("\n\n rdfxml_parse() will parse the following text:\n%s\n\n", text));
    }
  tf = tf_alloc ();
  tf->tf_qi = qi;
  tf->tf_default_graph_uri = graph_uri;
  tf->tf_app_env = app_env;
  tf->tf_creator = "rdf_load_rdfxml";
  tf->tf_input_name = source_name;
  tf->tf_line_no_ptr = &(parser->curr_pos.line_num);
  context.xp_tf = tf;
  QR_RESET_CTX
    {
      tf_set_cbk_names (tf, cbk_names);
      TF_CHANGE_GRAPH_TO_DEFAULT (tf);
      if (0 == setjmp (context.xp_error_ctx))
        rc = VXmlParse (parser, text, xrie.xrie_text_len);
      else
	rc = 0;
      tf_commit (tf);
    }
  QR_RESET_CODE
    {
      du_thread_t * self = THREAD_CURRENT_THREAD;
      caddr_t err = thr_get_error_code (self);
      POP_QR_RESET;
      VXmlParserDestroy (parser);
      xp_free (&context);
      if (NULL != xrie.xrie_iter_abend)
        xrie.xrie_iter_abend (xrie.xrie_iter_data);
      tf_free (tf);
      if (err_ret)
	*err_ret = err;
      else
	dk_free_tree (err);
      return;
    }
  END_QR_RESET;
  if (!rc)
    {
      caddr_t rc_msg = VXmlFullErrorMessage (parser);
      VXmlParserDestroy (parser);
      xp_free (&context);
      if (NULL != xrie.xrie_iter_abend)
        xrie.xrie_iter_abend (xrie.xrie_iter_data);
      tf_free (tf);
      if (err_ret)
	*err_ret = srv_make_new_error ("22007", "XM033", "%.1500s", rc_msg);
      dk_free_box (rc_msg);
      return;
    }
  XP_STRSES_FLUSH (&context);
/*
  if (NULL != ret_dtd)
    {
      ret_dtd[0] = VXmlGetDtd(parser);
      dtd_addref (ret_dtd[0], 0);
    }
  if (NULL != ret_id_cache)
    {
      ret_id_cache[0] = context.xp_id_dict;
      context.xp_id_dict = NULL;
    }
  if (NULL != ret_ns_2dict)
    {
      ret_ns_2dict[0] = parser->ns_2dict;
      parser->ns_2dict.xn2_size = 0;
    }
*/
  VXmlParserDestroy (parser);
  xp_free (&context);
  tf_free (tf);
  return;
}
