/**
 * The Seeks proxy and plugin framework are part of the SEEKS project.
 * Copyright (C) 2009-2011 Emmanuel Benazera, juban@free.fr
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "search_snippet.h"
#include "websearch.h" // for configuration.
#include "mem_utils.h"
#include "miscutil.h"
#include "encode.h"
#include "loaders.h"
#include "urlmatch.h"
#include "plugin_manager.h" // for _plugin_repository.
#include "seeks_proxy.h" // for _datadir.
#include "json_renderer.h"

#ifndef FEATURE_EXTENDED_HOST_PATTERNS
#include "proxy_dts.h" // for http_request.
#endif

#ifdef FEATURE_IMG_WEBSEARCH_PLUGIN
#include "img_search_snippet.h"
#endif

#if defined(PROTOBUF) && defined(TC)
#include "query_capture_configuration.h"
#endif

#include "static_renderer.h"

#include "mrf.h"

#include <ctype.h>
#include <iostream>

using sp::miscutil;
using sp::encode;
using sp::loaders;
using sp::urlmatch;
using sp::plugin_manager;
using sp::seeks_proxy;
using sp::http_request;
using lsh::mrf;

namespace seeks_plugins
{
  // loaded tagging patterns.
  std::vector<url_spec*> search_snippet::_pdf_pos_patterns = std::vector<url_spec*>();
  std::vector<url_spec*> search_snippet::_file_doc_pos_patterns = std::vector<url_spec*>();
  std::vector<url_spec*> search_snippet::_audio_pos_patterns = std::vector<url_spec*>();
  std::vector<url_spec*> search_snippet::_video_pos_patterns = std::vector<url_spec*>();
  std::vector<url_spec*> search_snippet::_forum_pos_patterns = std::vector<url_spec*>();
  std::vector<url_spec*> search_snippet::_reject_pos_patterns = std::vector<url_spec*>();

  search_snippet::search_snippet()
    :_qc(NULL),_new(true),_id(0),_sim_back(false),_rank(0),_seeks_ir(0.0),_meta_rank(0),_seeks_rank(0),
     _content_date(0),_record_date(0),_doc_type(WEBPAGE),
     _cached_content(NULL),_features(NULL),_features_tfidf(NULL),_bag_of_words(NULL),_safe(true),_personalized(false),_npeers(0),_hits(0),_radius(0)
  {
  }

  search_snippet::search_snippet(const short &rank)
    :_qc(NULL),_new(true),_id(0),_sim_back(false),_rank(rank),_seeks_ir(0.0),_meta_rank(0),_seeks_rank(0),
     _content_date(0),_record_date(0),_doc_type(WEBPAGE),
     _cached_content(NULL),_features(NULL),_features_tfidf(NULL),_bag_of_words(NULL),_safe(true),_personalized(false),_npeers(0),_hits(0),_radius(0)
  {
  }

  search_snippet::search_snippet(const search_snippet *s)
    :_qc(s->_qc),_new(s->_new),_id(s->_id),_title(s->_title),_url(s->_url),_cite(s->_cite),
     _cached(s->_cached),_summary(s->_summary),_summary_noenc(s->_summary_noenc),
     _file_format(s->_file_format),_date(s->_date),_lang(s->_lang),
     _archive(s->_archive),
     _sim_back(s->_sim_back),_rank(s->_rank),_meta_rank(s->_meta_rank),
     _seeks_rank(s->_seeks_rank),_content_date(s->_content_date),_record_date(s->_record_date),
     _engine(s->_engine),_doc_type(s->_doc_type),
     _forum_thread_info(s->_forum_thread_info),_cached_content(NULL),
     _features(NULL),_features_tfidf(NULL),_bag_of_words(NULL),_safe(s->_safe),_personalized(s->_personalized),
     _npeers(s->_npeers),_hits(s->_hits),_radius(s->_radius)
  {
    if (s->_cached_content)
      _cached_content = new std::string(*s->_cached_content);
    if (s->_features)
      _features = new std::vector<uint32_t>(*s->_features);
    if (s->_features_tfidf)
      _features_tfidf = new hash_map<uint32_t,float,id_hash_uint>(*s->_features_tfidf);
    if (s->_bag_of_words)
      _bag_of_words = new hash_map<uint32_t,std::string,id_hash_uint>(*s->_bag_of_words);
  }

  search_snippet::~search_snippet()
  {
    if (_cached_content)
      delete _cached_content;
    if (_features)
      delete _features;
    if (_features_tfidf)
      delete _features_tfidf;
    if (_bag_of_words)
      delete _bag_of_words;
  }

  void search_snippet::highlight_query(std::vector<std::string> &words,
                                       std::string &str)
  {
    if (words.empty())
      return;

    // sort words by size.
    std::sort(words.begin(),words.end(),std::greater<std::string>());

    // surround every of those words appearing within the
    // argument string with <b> </b> for html
    // bold format. TODO: green ?
    for (size_t i=0; i<words.size(); i++)
      {
        if (words.at(i).length() > 2)
          {
            std::string bold_str = "<b>" + words.at(i) + "</b>";
            miscutil::ci_replace_in_string(str,words.at(i),bold_str);
          }
      }
  }

  void search_snippet::discr_words(const std::vector<std::string> &query_words,
                                   std::set<std::string> &words) const
  {
    static int max_highlights = 3; // ad-hoc default.

    if (!_features_tfidf)
      return;

    std::map<float,uint32_t,std::greater<float> > f_tfidf;

    // sort features in decreasing tf-idf order.
    hash_map<uint32_t,float,id_hash_uint>::const_iterator fit
    = _features_tfidf->begin();
    while (fit!=_features_tfidf->end())
      {
        f_tfidf.insert(std::pair<float,uint32_t>((*fit).second,(*fit).first));
        ++fit;
      }

    size_t nqw = query_words.size();
    int i = 0;
    std::map<float,uint32_t,std::greater<float> >::const_iterator mit = f_tfidf.begin();
    while (mit!=f_tfidf.end())
      {
        hash_map<uint32_t,std::string,id_hash_uint>::const_iterator bit;
        if ((bit=_bag_of_words->find((*mit).second))!=_bag_of_words->end())
          {
            bool add = true;
            for (size_t j=0; j<nqw; j++)
              if (query_words.at(j) == (*bit).second)
                add = false;
            if (add)
              {
                for (size_t k=0; k<(*bit).second.length(); k++) // alphabetical characters only.
                  if (!isalpha((*bit).second[k]))
                    {
                      add = false;
                      break;
                    }
                if (add)
                  words.insert((*bit).second);
                i++;
              }
            if (i>=max_highlights)
              break;
          }
        ++mit;
      }

    if (words.empty())
      return;

    // sort words by size.
    //std::sort(words.begin(),words.end(),std::greater<std::string>());
  }

  std::string search_snippet::to_html(std::vector<std::string> &words,
                                      const std::string &base_url_str,
                                      const hash_map<const char*,const char*,hash<const char*>,eqstr> *parameters)
  {
#ifdef FEATURE_IMG_WEBSEARCH_PLUGIN
    // image snippet cast.
    img_search_snippet *isp = NULL;
    if (_doc_type == IMAGE)
      {
        isp = static_cast<img_search_snippet*>(this);
      }
#endif

    // check for URL redirection for capture & personalization of results.
    bool prs = true;
    const char *pers = miscutil::lookup(parameters,"prs");
    if (!pers)
      prs = websearch::_wconfig->_personalization;
    else
      {
        if (strcasecmp(pers,"on") == 0)
          prs = true;
        else if (strcasecmp(pers,"off") == 0)
          prs = false;
        else prs = websearch::_wconfig->_personalization;
      }

    std::string url = _url;
    char *url_encp = encode::url_encode(url.c_str());
    std::string url_enc = std::string(url_encp);
    free(url_encp);

#if defined(PROTOBUF) && defined(TC)
    if (prs && websearch::_qc_plugin && websearch::_qc_plugin_activated
        && query_capture_configuration::_config)
      {
        std::string redir = "/qc_redir";
        if (_doc_type == IMAGE)
          redir += "_img";
        url = base_url_str + redir + "?q=" + _qc->_url_enc_query + "&amp;url=" + url_enc
              + "&amp;lang=" + _qc->_auto_lang;
      }
#endif

    std::string html_content = "<li class=\"search_snippet";
    if (_doc_type == VIDEO_THUMB)
      html_content += " search_snippet_vid";
    else if (_doc_type == IMAGE)
      html_content += " search_snippet_img";
    html_content += "\">";
    const char *thumbs = miscutil::lookup(parameters,"thumbs");
    bool has_thumbs = websearch::_wconfig->_thumbs;
    if (thumbs && strcasecmp(thumbs,"on") == 0)
      has_thumbs = true;
    if (_doc_type != TWEET && _doc_type != IMAGE && _doc_type != VIDEO_THUMB && has_thumbs)
      {
        html_content += "<a href=\"" + url + "\">";
        html_content += "<img class=\"preview\" src=\"http://open.thumbshots.org/image.pxf?url=";
        html_content += _url;
        html_content += "\" /></a>";
      }
    if (_doc_type == TWEET)
      {
        html_content += "<a href=\"" + _cite + "\">";
        html_content += "<img class=\"tweet_profile\" src=\"" + _cached + "\" ></a>"; // _cached contains the profile's image.
      }
    if (_doc_type == VIDEO_THUMB)
      {
        html_content += "<a href=\"";
        html_content += url + "\"><img class=\"video_profile\" src=\"";
        html_content += _cached;
        html_content += "\"></a><div>";
      }
    else if (_doc_type == IMAGE)
      {
        html_content += "<a href=\"";
        html_content += url + "\"><img src=\"";
        html_content += _cached;
        html_content += "\"></a><div>";
      }

    if (prs && _personalized && !_engine.has_feed("seeks"))
      {
        html_content += "<h3 class=\"personalized_result personalized\" title=\"personalized result\">";
      }
    else html_content += "<h3>";
    html_content += "<a href=\"";
    html_content += url;
    html_content += "\">";

    char* title_enc = encode::html_encode(_title.c_str());
    html_content += std::string(title_enc);
    free(title_enc);
    html_content += "</a>";

    std::string se_icon = "<span class=\"search_engine icon\" title=\"setitle\"><a href=\"" + base_url_str
                          + "/search";
    if (_doc_type == IMAGE)
      se_icon += "_img";
    se_icon += "?q=@query@?page=1&amp;expansion=1&amp;engines=seeng&amp;lang="
               + _qc->_auto_lang + "&amp;ui=stat\">&nbsp;</a></span>";
    if (_engine.has_feed("google"))
      {
        std::string ggle_se_icon = se_icon;
        miscutil::replace_in_string(ggle_se_icon,"icon","search_engine_google");
        miscutil::replace_in_string(ggle_se_icon,"setitle","Google");
        miscutil::replace_in_string(ggle_se_icon,"seeng","google");
        miscutil::replace_in_string(ggle_se_icon,"@query@",_qc->_url_enc_query);
        html_content += ggle_se_icon;
      }
    if (_engine.has_feed("bing"))
      {
        std::string bing_se_icon = se_icon;
        miscutil::replace_in_string(bing_se_icon,"icon","search_engine_bing");
        miscutil::replace_in_string(bing_se_icon,"setitle","Bing");
        miscutil::replace_in_string(bing_se_icon,"seeng","bing");
        miscutil::replace_in_string(bing_se_icon,"@query@",_qc->_url_enc_query);
        html_content += bing_se_icon;
      }
    if (_engine.has_feed("blekko"))
      {
        std::string blekko_se_icon = se_icon;
        miscutil::replace_in_string(blekko_se_icon,"icon","search_engine_blekko");
        miscutil::replace_in_string(blekko_se_icon,"setitle","blekko!");
        miscutil::replace_in_string(blekko_se_icon,"seeng","blekko");
        miscutil::replace_in_string(blekko_se_icon,"@query@",_qc->_url_enc_query);
        html_content += blekko_se_icon;
      }
    if (_engine.has_feed("yauba"))
      {
        std::string yauba_se_icon = se_icon;
        miscutil::replace_in_string(yauba_se_icon,"icon","search_engine_yauba");
        miscutil::replace_in_string(yauba_se_icon,"setitle","yauba!");
        miscutil::replace_in_string(yauba_se_icon,"seeng","yauba");
        miscutil::replace_in_string(yauba_se_icon,"@query@",_qc->_url_enc_query);
        html_content += yauba_se_icon;
      }
    if (_engine.has_feed("yahoo"))
      {
        std::string yahoo_se_icon = se_icon;
        miscutil::replace_in_string(yahoo_se_icon,"icon","search_engine_yahoo");
        miscutil::replace_in_string(yahoo_se_icon,"setitle","Yahoo!");
        miscutil::replace_in_string(yahoo_se_icon,"seeng","yahoo");
        miscutil::replace_in_string(yahoo_se_icon,"@query@",_qc->_url_enc_query);
        html_content += yahoo_se_icon;
      }
    if (_engine.has_feed("exalead"))
      {
        std::string exalead_se_icon = se_icon;
        miscutil::replace_in_string(exalead_se_icon,"icon","search_engine_exalead");
        miscutil::replace_in_string(exalead_se_icon,"setitle","Exalead");
        miscutil::replace_in_string(exalead_se_icon,"seeng","exalead");
        miscutil::replace_in_string(exalead_se_icon,"@query@",_qc->_url_enc_query);
        html_content += exalead_se_icon;
      }
    if (_engine.has_feed("twitter"))
      {
        std::string twitter_se_icon = se_icon;
        miscutil::replace_in_string(twitter_se_icon,"icon","search_engine_twitter");
        miscutil::replace_in_string(twitter_se_icon,"setitle","Twitter");
        miscutil::replace_in_string(twitter_se_icon,"seeng","twitter");
        miscutil::replace_in_string(twitter_se_icon,"@query@",_qc->_url_enc_query);
        html_content += twitter_se_icon;
      }
    if (_engine.has_feed("dailymotion"))
      {
        std::string yt_se_icon = se_icon;
        miscutil::replace_in_string(yt_se_icon,"icon","search_engine_dailymotion");
        miscutil::replace_in_string(yt_se_icon,"setitle","Dailymotion");
        miscutil::replace_in_string(yt_se_icon,"seeng","Dailymotion");
        miscutil::replace_in_string(yt_se_icon,"@query@",_qc->_url_enc_query);
        html_content += yt_se_icon;
      }
    if (_engine.has_feed("youtube"))
      {
        std::string yt_se_icon = se_icon;
        miscutil::replace_in_string(yt_se_icon,"icon","search_engine_youtube");
        miscutil::replace_in_string(yt_se_icon,"setitle","Youtube");
        miscutil::replace_in_string(yt_se_icon,"seeng","youtube");
        miscutil::replace_in_string(yt_se_icon,"@query@",_qc->_url_enc_query);
        html_content += yt_se_icon;
      }
    if (_engine.has_feed("dokuwiki"))
      {
        std::string dk_se_icon = se_icon;
        miscutil::replace_in_string(dk_se_icon,"icon","search_engine_dokuwiki");
        miscutil::replace_in_string(dk_se_icon,"setitle","Dokuwiki");
        miscutil::replace_in_string(dk_se_icon,"seeng","dokuwiki");
        miscutil::replace_in_string(dk_se_icon,"@query@",_qc->_url_enc_query);
        html_content += dk_se_icon;
      }
    if (_engine.has_feed("mediawiki"))
      {
        std::string md_se_icon = se_icon;
        miscutil::replace_in_string(md_se_icon,"icon","search_engine_mediawiki");
        miscutil::replace_in_string(md_se_icon,"setitle","Mediawiki");
        miscutil::replace_in_string(md_se_icon,"seeng","mediawiki");
        miscutil::replace_in_string(md_se_icon,"@query@",_qc->_url_enc_query);
        html_content += md_se_icon;
      }
    if (_engine.has_feed("opensearch_rss") || _engine.has_feed("opensearch_atom"))
      {
        std::string md_se_icon = se_icon;
        miscutil::replace_in_string(md_se_icon,"icon","search_engine_opensearch");
        miscutil::replace_in_string(md_se_icon,"setitle","Opensearch");
        miscutil::replace_in_string(md_se_icon,"seeng","opensearch");
        miscutil::replace_in_string(md_se_icon,"@query@",_qc->_url_enc_query);
        html_content += md_se_icon;
      }
#ifdef FEATURE_IMG_WEBSEARCH_PLUGIN
    if (isp)
      {
        if (isp->_img_engine.has_feed("bing_img"))
          {
            std::string sk_se_icon = se_icon;
            miscutil::replace_in_string(sk_se_icon,"icon","search_engine_bing");
            miscutil::replace_in_string(sk_se_icon,"setitle","Bing");
            miscutil::replace_in_string(sk_se_icon,"seeng","bing");
            miscutil::replace_in_string(sk_se_icon,"@query@",_qc->_url_enc_query);
            html_content += sk_se_icon;
          }
        if (isp->_img_engine.has_feed("flickr"))
          {
            std::string sk_se_icon = se_icon;
            miscutil::replace_in_string(sk_se_icon,"icon","search_engine_flickr");
            miscutil::replace_in_string(sk_se_icon,"setitle","Flickr");
            miscutil::replace_in_string(sk_se_icon,"seeng","flickr");
            miscutil::replace_in_string(sk_se_icon,"@query@",_qc->_url_enc_query);
            html_content += sk_se_icon;
          }
        if (isp->_img_engine.has_feed("google_img"))
          {
            std::string sk_se_icon = se_icon;
            miscutil::replace_in_string(sk_se_icon,"icon","search_engine_ggle");
            miscutil::replace_in_string(sk_se_icon,"setitle","Google");
            miscutil::replace_in_string(sk_se_icon,"seeng","google");
            miscutil::replace_in_string(sk_se_icon,"@query@",_qc->_url_enc_query);
            html_content += sk_se_icon;
          }
        if (isp->_img_engine.has_feed("wcommons"))
          {
            std::string sk_se_icon = se_icon;
            miscutil::replace_in_string(sk_se_icon,"icon","search_engine_wcommons");
            miscutil::replace_in_string(sk_se_icon,"setitle","WikiCommons");
            miscutil::replace_in_string(sk_se_icon,"seeng","wiki commons");
            miscutil::replace_in_string(sk_se_icon,"@query@",_qc->_url_enc_query);
            html_content += sk_se_icon;
          }
        if (isp->_img_engine.has_feed("yahoo_img"))
          {
            std::string sk_se_icon = se_icon;
            miscutil::replace_in_string(sk_se_icon,"icon","search_engine_yahoo");
            miscutil::replace_in_string(sk_se_icon,"setitle","Yahoo");
            miscutil::replace_in_string(sk_se_icon,"seeng","yahoo");
            miscutil::replace_in_string(sk_se_icon,"@query@",_qc->_url_enc_query);
            html_content += sk_se_icon;
          }
      } // end image sp.
#endif
    if (_engine.has_feed("seeks"))
      {
        std::string sk_se_icon = se_icon;
        miscutil::replace_in_string(sk_se_icon,"icon","search_engine_seeks");
        miscutil::replace_in_string(sk_se_icon,"setitle","Seeks");
        miscutil::replace_in_string(sk_se_icon,"seeng","seeks");
        miscutil::replace_in_string(sk_se_icon,"@query@",_qc->_url_enc_query);
        html_content += sk_se_icon;
      }

    if (_doc_type == TWEET)
      if (_meta_rank > 1)
        html_content += " (" + miscutil::to_string(_meta_rank) + ")";
    html_content += "</h3>";

    if (!_summary.empty())
      {
        html_content += "<div>";
        std::string summary = _summary;
        search_snippet::highlight_query(words,summary);
        if (websearch::_wconfig->_extended_highlight)
          static_renderer::highlight_discr(this,summary,base_url_str,words);
        html_content += summary;
      }
    else html_content += "<div>";

    const char *cite_enc = NULL;
    if (_doc_type != VIDEO_THUMB)
      {
        if (!_cite.empty())
          {
            cite_enc = encode::html_encode(_cite.c_str());
          }
        else
          {
            cite_enc = encode::html_encode(_url.c_str());
          }
      }
    else
      {
        cite_enc = encode::html_encode(_date.c_str());
      }
    if (!_summary.empty())
      html_content += "<br>";
    html_content += "<a class=\"search_cite\" href=\"" + _url + "\">";
    html_content += "<cite>";
    html_content += cite_enc;
    free_const(cite_enc);
    html_content += "</cite></a>";

    if (!_cached.empty() && _doc_type != TWEET && _doc_type != VIDEO_THUMB)
      {
        html_content += "\n";
        char *enc_cached = encode::html_encode(_cached.c_str());
        miscutil::chomp(enc_cached);
        html_content += "<a class=\"search_cache\" href=\"";
        html_content += enc_cached;
        html_content += "\">Cached</a>";
        free_const(enc_cached);
      }
    else if (_doc_type == TWEET)
      {
        char *date_enc = encode::html_encode(_date.c_str());
        html_content += "<date> (";
        html_content += date_enc;
        free_const(date_enc);
        html_content += ") </date>\n";
      }
    if (_doc_type != TWEET && _doc_type != VIDEO_THUMB)
      {
        if (_archive.empty())
          {
            set_archive_link();
          }
        html_content += "<a class=\"search_cache\" href=\"";
        html_content += _archive;
        html_content += "\">Archive</a>";
      }

    if (_doc_type != VIDEO_THUMB)
      {
        std::string sim_link;
        const char *engines = miscutil::lookup(parameters,"engines");
        if (!_sim_back)
          {
            sim_link = "/search";
            if (_doc_type == IMAGE)
              sim_link += "_img";
            sim_link += "?q=" + _qc->_url_enc_query + "&amp;id=" + miscutil::to_string(_id)
                        + "&amp;page=1&amp;expansion=" + miscutil::to_string(_qc->_page_expansion)
                        + "&amp;lang=" + _qc->_auto_lang
                        + "&amp;ui=stat&amp;action=similarity";
            if (engines)
              sim_link += "&amp;engines=" + std::string(engines);
            set_similarity_link(parameters);
            html_content += "<a class=\"search_cache\" href=\"";
          }
        else
          {
            sim_link = "/search";
            if (_doc_type == IMAGE)
              sim_link += "_img";
            sim_link += "?q=" + _qc->_url_enc_query
                        + "&amp;page=1&amp;expansion=" + miscutil::to_string(_qc->_page_expansion)
                        + "&amp;lang=" + _qc->_auto_lang
                        + "&amp;ui=stat&amp;action=expand";
            if (engines)
              sim_link += "&amp;engines=" + std::string(engines);
            set_back_similarity_link(parameters);
            html_content += "<a class=\"search_similarity\" href=\"";
          }
        html_content += base_url_str + sim_link;
        if (!_sim_back)
          html_content += "\">Similar</a>";
        else html_content += "\">Back</a>";
      }

    if (_cached_content)
      {
        html_content += "<a class=\"search_cache\" href=\"";
        html_content += base_url_str + "/search_cache?url="
                        + _url + "&amp;q=" + _qc->_query;
        html_content += " \">Quick link</a>";
      }

    // snippet type rendering
    const char *engines = miscutil::lookup(parameters,"engines");
    if (_doc_type != REJECTED)
      {
        html_content += "<a class=\"search_cache\" href=\"";
        html_content += base_url_str + "/cluster/types/" + _qc->_url_enc_query
                        + "?expansion=xxexp&amp;ui=stat&amp;engines=";
        if (engines)
          html_content += std::string(engines);
        html_content += " \"> ";
        switch (_doc_type)
          {
          case UNKNOWN:
            html_content += "";
            break;
          case WEBPAGE:
            html_content += "Webpage";
            break;
          case FORUM:
            html_content += "Forum";
            break;
          case FILE_DOC:
            html_content += "Document file";
            break;
          case SOFTWARE:
            html_content += "Software";
            break;
          case IMAGE:
            html_content += "Image";
            break;
          case VIDEO:
            html_content += "Video";
            break;
          case VIDEO_THUMB:
            html_content += "Video";
            break;
          case AUDIO:
            html_content += "Audio";
            break;
          case CODE:
            html_content += "Code";
            break;
          case NEWS:
            html_content += "News";
            break;
          case TWEET:
            html_content += "Tweet";
            break;
          case WIKI:
            html_content += "Wiki";
            break;
          case POST:
            html_content += "Post";
            break;
          case BUG:
            html_content += "Bug";
            break;
          case ISSUE:
            html_content += "Issue";
            break;
          case REVISION:
            html_content += "Revision";
            break;
          case COMMENT:
            html_content += "Comment";
            break;
          case REJECTED:
            break;
          }
        html_content += "</a>";
      }

#if defined(PROTOBUF) && defined(TC)
    // snippet thumb down rendering
    if (_personalized)
      {
        html_content += "<a class=\"search_tbd\" title=\"reject personalized result\" href=\"" + base_url_str + "/tbd?q="
                        + _qc->_url_enc_query + "&amp;url=" + url_enc + "&amp;action=expand&amp;expansion=xxexp&amp;ui=stat&amp;engines=";
        if (engines)
          html_content += std::string(engines);
        html_content += "&amp;lang=" + _qc->_auto_lang;
        html_content += "\">&nbsp;</a>";
        if (_hits > 0 && _npeers > 0)
          html_content += "<br><div class=\"snippet_info\">" + miscutil::to_string(_hits)
                          + " recommendation(s) by " + miscutil::to_string(_npeers) + " peer(s).</div>";
      }
#endif

    html_content += "</div></li>\n";
    return html_content;
  }

  std::string search_snippet::to_json(const bool &thumbs,
                                      const std::vector<std::string> &query_words)
  {
    std::string json_str;
    json_str += "{";
    json_str += "\"id\":" + miscutil::to_string(_id) + ",";
    std::string title = _title;
    miscutil::replace_in_string(title,"\\","\\\\");
    miscutil::replace_in_string(title,"\"","\\\"");
    json_str += "\"title\":\"" + title + "\",";
    std::string url = _url;
    miscutil::replace_in_string(url,"\"","\\\"");
    miscutil::replace_in_string(url,"\n","");
    json_str += "\"url\":\"" + url + "\",";
    std::string summary = _summary;
    miscutil::replace_in_string(summary,"\\","\\\\");
    miscutil::replace_in_string(summary,"\"","\\\"");
    json_str += "\"summary\":\"" + summary + "\",";
    json_str += "\"seeks_meta\":" + miscutil::to_string(_meta_rank) + ",";
    json_str += "\"seeks_score\":" + miscutil::to_string(_seeks_rank) + ",";
    double rank = 0.0;
    if (_engine.size() > 0)
      rank = _rank / static_cast<double>(_engine.size());
    json_str += "\"rank\":" + miscutil::to_string(rank) + ",";
    json_str += "\"cite\":\"";
    if (!_cite.empty())
      {
        std::string cite = _cite;
        miscutil::replace_in_string(cite,"\"","\\\"");
        miscutil::replace_in_string(cite,"\n","");
        json_str += cite + "\",";
      }
    else json_str += url + "\",";
    if (!_cached.empty())
      {
        std::string cached = _cached;
        miscutil::replace_in_string(cached,"\"","\\\"");
        json_str += "\"cached\":\"" + cached + "\","; // XXX: cached might be malformed without preprocessing.
      }
    json_str += "\"engines\":[";
#ifdef FEATURE_IMG_WEBSEARCH_PLUGIN
    img_search_snippet *isp = NULL;
    if (_doc_type == IMAGE)
      isp = static_cast<img_search_snippet*>(this);
    if (isp)
      json_str += json_renderer::render_engines(isp->_img_engine,true);
    else
#endif
      json_str += json_renderer::render_engines(_engine);
    json_str += "]";
    if (thumbs)
      json_str += ",\"thumb\":\"http://open.thumbshots.org/image.pxf?url=" + url + "\"";
    std::set<std::string> words;
    discr_words(query_words,words);
    if (!words.empty())
      {
        json_str += ",\"words\":[";
        //json_str += miscutil::join_string_list(",",words);
        std::set<std::string>::const_iterator sit = words.begin();
        while(sit!=words.end())
          {
            json_str += "\"" + (*sit) + "\"";
            if (sit != --words.end())
              json_str += ",";
            ++sit;
          }
        json_str += "]";
      }
    json_str += ",\"type\":\"" + get_doc_type_str() + "\"";
    json_str += ",\"personalized\":\"";
    if (_personalized)
      json_str += "yes";
    else json_str += "no";
    json_str += "\"";
    if (_npeers > 0)
      json_str += ",\"snpeers\":" + miscutil::to_string(_npeers);
    if (_hits > 0)
      json_str += ",\"hits\":" + miscutil::to_string(_hits);
    if (!_date.empty())
      json_str += ",\"date\":\"" + _date + "\"";

    json_str += "}";
    return json_str;
  }

  std::ostream& search_snippet::print(std::ostream &output)
  {
    output << "-----------------------------------\n";
    output << "- seeks rank: " << _meta_rank << std::endl;
    output << "- rank: " << _rank << std::endl;
    output << "- title: " << _title << std::endl;
    output << "- url: " << _url << std::endl;
    output << "- cite: " << _cite << std::endl;
    output << "- cached: " << _cached << std::endl;
    output << "- summary: " << _summary << std::endl;
    output << "- file format: " << _file_format << std::endl;
    output << "- date: " << _date << std::endl;
    output << "- lang: " << _lang << std::endl;
    if (_doc_type == FORUM)
      output << "- forum thread info: " << _forum_thread_info << std::endl;
    output << "-----------------------------------\n";

    return output;
  }


  bool search_snippet::is_se_enabled(const hash_map<const char*,const char*,hash<const char*>,eqstr> *parameters)
  {
    if (_personalized && _engine.has_feed("seeks"))
      return true;
    feeds se_enabled;
    query_context::fillup_engines(parameters,se_enabled);
    feeds band = _engine.inter(se_enabled);

    /*if (band.empty())
      {
        // check for a wildcard (all feeds for a given parser).
        band = _engine.inter_gen(se_enabled);
    	}*/
    return (band.size() != 0);
  }

  void search_snippet::set_title(const std::string &title)
  {
    _title = encode::html_decode(title);
    miscutil::replace_in_string(_title,"\\","");
    miscutil::replace_in_string(_title,"\t"," ");
    miscutil::replace_in_string(_title,"\n"," ");
    miscutil::replace_in_string(_title,"\r"," ");
  }

  void search_snippet::set_title_no_html_decode(const std::string &title)
  {
    _title = title;
    miscutil::replace_in_string(_title,"\\","");
    miscutil::replace_in_string(_title,"\t"," ");
    miscutil::replace_in_string(_title,"\n"," ");
    miscutil::replace_in_string(_title,"\r"," ");
  }

  void search_snippet::set_url(const std::string &url)
  {
    char *url_str = encode::url_decode_but_not_plus(url.c_str());
    _url = std::string(url_str);
    free(url_str);
    std::string url_lc(_url);
    miscutil::to_lower(url_lc);
    std::string surl = urlmatch::strip_url(url_lc);
    _id = mrf::mrf_single_feature(surl);
  }

  void search_snippet::set_url_no_decode(const std::string &url)
  {
    _url = url;
    std::string url_lc(_url);
    miscutil::to_lower(url_lc);
    std::string surl = urlmatch::strip_url(url_lc);
    _id = mrf::mrf_single_feature(surl);
  }

  void search_snippet::set_cite(const std::string &cite)
  {
    char *cite_dec = encode::url_decode_but_not_plus(cite.c_str());
    std::string citer = std::string(cite_dec);
    free(cite_dec);
    static size_t cite_max_size = 60;
    _cite = urlmatch::strip_url(citer);
    if (_cite.length()>cite_max_size)
      {
        try
          {
            _cite.substr(0,cite_max_size-3) + "...";
          }
        catch (std::exception &e)
          {
            // do nothing.
          }
      }
  }

  void search_snippet::set_cite_no_decode(const std::string &cite)
  {
    static size_t cite_max_size = 60;
    _cite = urlmatch::strip_url(cite);
    if (_cite.length()>cite_max_size)
      {
        try
          {
            _cite.substr(0,cite_max_size-3) + "...";
          }
        catch (std::exception &e)
          {
            // do nothing.
          }
      }
  }

  void search_snippet::set_summary(const std::string &summary)
  {
    static size_t summary_max_size = 240; // characters.
    _summary_noenc = summary;

    // clear escaped characters for unencoded output.
    miscutil::replace_in_string(_summary_noenc,"\\","");

    // encode html so tags are not interpreted.
    char* str = encode::html_encode(summary.c_str());
    if (strlen(str)<summary_max_size)
      _summary = std::string(str);
    else
      {
        try
          {
            _summary = std::string(str).substr(0,summary_max_size-3) + "...";
          }
        catch (std::exception &e)
          {
            _summary = "";
          }
      }
    free(str);
  }

  void search_snippet::set_date(const std::string &date)
  {
    size_t p = date.find("+");
    if (p != std::string::npos)
      {
        _date = date.substr(0,p-1);
      }
    else _date = date;
  }

  void search_snippet::set_lang(const std::string &lang)
  {
    _lang = lang;
  }

  void search_snippet::set_radius(const int &radius)
  {
    _radius = radius;
  }

  void search_snippet::set_archive_link()
  {
    _archive = "http://web.archive.org/web/*/" + _url;
  }

  void search_snippet::set_similarity_link(const hash_map<const char*,const char*,hash<const char*>,eqstr> *parameters)
  {
    _sim_back = false;
  }

  void search_snippet::set_back_similarity_link(const hash_map<const char*,const char*,hash<const char*>,eqstr> *parameters)
  {
    _sim_back = true;
  }

  std::string search_snippet::get_stripped_url() const
  {
    std::string url_lc(_url);
    miscutil::to_lower(url_lc);
    std::string surl = urlmatch::strip_url(url_lc);
    return surl;
  }

  void search_snippet::tag()
  {
    // detect extension, if any, and if not already tagged.
    if (_doc_type == WEBPAGE) // not already tagged.
      {
        // grab the 3 char long extension, if any.
        std::string file_ext;
        if (_url.size()>4 && _url[_url.size()-4] == '.')
          {
            try
              {
                file_ext = _url.substr(_url.size()-3);
              }
            catch (std::exception &e)
              {
                file_ext = "";
              }
            _file_format = file_ext;
          }

        if (search_snippet::match_tag(_url,search_snippet::_pdf_pos_patterns))
          _doc_type = FILE_DOC;
        else if (search_snippet::match_tag(_url,search_snippet::_file_doc_pos_patterns))
          _doc_type = FILE_DOC;
        else if (search_snippet::match_tag(_url,search_snippet::_audio_pos_patterns))
          _doc_type = AUDIO;
        else if (search_snippet::match_tag(_url,search_snippet::_video_pos_patterns))
          _doc_type = VIDEO;
        else if (search_snippet::match_tag(_url,search_snippet::_forum_pos_patterns))
          _doc_type = FORUM;
        else if (search_snippet::match_tag(_url,search_snippet::_reject_pos_patterns))
          _doc_type = REJECTED;

        /* std::cerr << "[Debug]: tagged snippet: url: " << _url
          << " -- tag: " << (int)_doc_type << std::endl; */
      }

    // detect wikis. XXX: could be put into a pattern file if more complex patterns are needed.
    if (_doc_type == WEBPAGE)
      {
        size_t pos = 0;
        std::string wiki_pattern = "wiki";
        std::string::const_iterator sit = _url.begin();
        if ((pos = miscutil::ci_find(_url,wiki_pattern,sit))!=std::string::npos)
          {
            _doc_type = WIKI;
          }
      }
  }

  // static.
  sp_err search_snippet::load_patterns()
  {
    static std::string pdf_patterns_filename
    = (seeks_proxy::_datadir.empty()) ? plugin_manager::_plugin_repository + "websearch/patterns/pdf"
      : seeks_proxy::_datadir + "/plugins/websearch/patterns/pdf";
    static std::string file_doc_patterns_filename
    = (seeks_proxy::_datadir.empty()) ? plugin_manager::_plugin_repository + "websearch/patterns/file_doc"
      : seeks_proxy::_datadir + "/plugins/websearch/patterns/file_doc";
    static std::string audio_patterns_filename
    = (seeks_proxy::_datadir.empty()) ? plugin_manager::_plugin_repository + "websearch/patterns/audio"
      : seeks_proxy::_datadir + "/plugins/websearch/patterns/audio";
    static std::string video_patterns_filename
    = (seeks_proxy::_datadir.empty()) ? plugin_manager::_plugin_repository + "websearch/patterns/video"
      : seeks_proxy::_datadir + "/plugins/websearch/patterns/video";
    static std::string forum_patterns_filename
    = (seeks_proxy::_datadir.empty()) ? plugin_manager::_plugin_repository + "websearch/patterns/forum"
      : seeks_proxy::_datadir + "/plugins/websearch/patterns/forum";
    static std::string reject_patterns_filename
    = (seeks_proxy::_datadir.empty()) ? plugin_manager::_plugin_repository + "websearch/patterns/reject"
      : seeks_proxy::_datadir + "/plugins/websearch/patterns/reject";

    std::vector<url_spec*> fake_neg_patterns; // XXX: maybe to be supported in the future, if needed.

    sp_err err;
    err = loaders::load_pattern_file(pdf_patterns_filename.c_str(),search_snippet::_pdf_pos_patterns,
                                     fake_neg_patterns);
    if (err == SP_ERR_OK)
      err = loaders::load_pattern_file(file_doc_patterns_filename.c_str(),search_snippet::_file_doc_pos_patterns,
                                       fake_neg_patterns);
    if (err == SP_ERR_OK)
      err = loaders::load_pattern_file(audio_patterns_filename.c_str(),search_snippet::_audio_pos_patterns,
                                       fake_neg_patterns);
    if (err == SP_ERR_OK)
      err = loaders::load_pattern_file(video_patterns_filename.c_str(),search_snippet::_video_pos_patterns,
                                       fake_neg_patterns);
    if (err == SP_ERR_OK)
      err = loaders::load_pattern_file(forum_patterns_filename.c_str(),search_snippet::_forum_pos_patterns,
                                       fake_neg_patterns);
    if (err == SP_ERR_OK)
      err = loaders::load_pattern_file(reject_patterns_filename.c_str(),search_snippet::_reject_pos_patterns,
                                       fake_neg_patterns);
    return err;
  }

  void search_snippet::destroy_patterns()
  {
    std::for_each(_pdf_pos_patterns.begin(),_pdf_pos_patterns.end(),delete_object());
    std::for_each(_file_doc_pos_patterns.begin(),_file_doc_pos_patterns.end(),delete_object());
    std::for_each(_audio_pos_patterns.begin(),_audio_pos_patterns.end(),delete_object());
    std::for_each(_video_pos_patterns.begin(),_video_pos_patterns.end(),delete_object());
    std::for_each(_forum_pos_patterns.begin(),_forum_pos_patterns.end(),delete_object());
    std::for_each(_reject_pos_patterns.begin(),_reject_pos_patterns.end(),delete_object());
  }

  bool search_snippet::match_tag(const std::string &url,
                                 const std::vector<url_spec*> &patterns)
  {
    std::string host;
    std::string path;
    urlmatch::parse_url_host_and_path(url,host,path);

    /* std::cerr << "url: " << url << std::endl;
    std::cerr << "[Debug]: host: " << host << " -- path: " << path
      << " -- pattern size: " << patterns.size() << std::endl; */

#ifndef FEATURE_EXTENDED_HOST_PATTERNS
    http_request http;
    http._host = (char*)host.c_str();
    urlmatch::init_domain_components(&http);
#endif

    size_t psize = patterns.size();
    for (size_t i=0; i<psize; i++)
      {
        url_spec *pattern = patterns.at(i);

        // host matching.
#ifdef FEATURE_EXTENDED_HOST_PATTERNS
        int host_match = host.empty() ? 0 : ((NULL == pattern->_host_regex)
                                             || (0 == regexec(pattern->_host_regex, host.c_str(), 0, NULL, 0)));
#else
        int host_match = urlmatch::host_matches(&http,pattern);
#endif
        if (host_match == 0)
          continue;

        // path matching.
        int path_match = urlmatch::path_matches(path.c_str(),pattern);
        if (path_match)
          {
#ifndef FEATURE_EXTENDED_HOST_PATTERNS
            http._host = NULL;
#endif
            return true;
          }
      }
#ifndef FEATURE_EXTENDED_HOST_PATTERNS
    http._host = NULL;
#endif
    return false;
  }

  void search_snippet::merge_snippets(search_snippet *s1,
                                      const search_snippet *s2)
  {
    if (s1->_doc_type != TWEET)
      {
        if (s1->_engine.equal(s2->_engine))
          return;
      }

    // search engine rank.
    s1->_rank += s2->_rank;

    // search engine.
    s1->_engine = s1->_engine.sunion(s2->_engine);

    // seeks_rank
    s1->_seeks_rank += s2->_seeks_rank;

    // cached link.
    if (s1->_cached.empty())
      s1->_cached = s2->_cached;

    // summary.
    if (s1->_summary.length() < s2->_summary.length())
      s1->_summary = s2->_summary;

    // cite.
    if (s1->_cite.length() > s2->_cite.length())
      s1->_cite = s2->_cite;

    // snippet type: more specialize type wins.
    // for now, very basic.
    s1->_doc_type = std::max(s1->_doc_type,s2->_doc_type);

    // TODO: merge dates.

    // file format.
    if (s1->_file_format.length() < s2->_file_format.length())  // we could do better here, ok enough for now.
      s1->_file_format = s2->_file_format;

    // meta rank.
    if (s1->_doc_type == TWEET)
      {
        if (s1->_meta_rank <= 0)
          s1->_meta_rank++;
        s1->_meta_rank++; // similarity detects retweets and merges them.
      }
    else
      {
        s1->_meta_rank = s1->_engine.size();
        s1->bing_yahoo_us_merge();
      }

    // radius.
    s1->_radius = std::min(s1->_radius,s2->_radius);
  }

  void search_snippet::bing_yahoo_us_merge()
  {
    // XXX: hack, on English queries, Bing & Yahoo are the same engine,
    // therefore the rank must be tweaked accordingly in this special case.
    if (_qc->_auto_lang == "en"
        && _engine.has_feed("yahoo")
        && _engine.has_feed("bing"))
      _meta_rank--;
  }

  void search_snippet::reset_p2p_data()
  {
    if (_engine.has_feed("seeks"))
      _engine.remove_feed("seeks");
    _meta_rank = _engine.size();
    _seeks_rank = 0;
    _npeers = 0;
    _hits = 0;
  }

  std::string search_snippet::get_doc_type_str() const
  {
    std::string output;
    switch (_doc_type)
      {
      case WEBPAGE:
        output = "webpage";
        break;
      case FORUM:
        output = "forum";
        break;
      case FILE_DOC:
        output = "file";
        break;
      case SOFTWARE:
        output = "software";
        break;
      case IMAGE:
        output = "image";
        break;
      case VIDEO:
        output = "video";
        break;
      case VIDEO_THUMB:
        output = "video_thumb";
        break;
      case AUDIO:
        output = "audio";
        break;
      case CODE:
        output = "code";
        break;
      case NEWS:
        output = "news";
        break;
      case TWEET:
        output = "tweet";
        break;
      case WIKI:
        output = "wiki";
        break;
      case UNKNOWN:
      default:
        output = "unknown";
      }
    return output;
  }

} /* end of namespace. */
