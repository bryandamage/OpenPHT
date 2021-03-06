#include "PlexFile.h"
#include "Client/PlexServerManager.h"
#include "utils/log.h"
#include "settings/GUISettings.h"
#include "boost/lexical_cast.hpp"
#include <boost/assign.hpp>
#include <boost/foreach.hpp>
#include <string>
#include "Mime.h"
#include "URIUtils.h"
#include "filesystem/File.h"

#include "PlexApplication.h"
#include "GUIInfoManager.h"
#include "LangInfo.h"

using namespace XFILE;
using namespace std;

typedef pair<string, string> stringPair;

///////////////////////////////////////////////////////////////////////////////////////////////////
vector<stringPair> CPlexFile::GetHeaderList()
{
  std::vector<std::pair<std::string, std::string> > hdrs;
  
  hdrs.push_back(stringPair("X-Plex-Version", g_infoManager.GetVersion()));
  hdrs.push_back(stringPair("X-Plex-Client-Identifier", g_guiSettings.GetString("system.uuid")));
  hdrs.push_back(stringPair("X-Plex-Product", "Plex Home Theater"));
  hdrs.push_back(stringPair("X-Plex-Device-Name", g_guiSettings.GetString("services.devicename")));
  hdrs.push_back(stringPair("X-Plex-Platform", "Plex Home Theater"));
  hdrs.push_back(stringPair("X-Plex-Model", PlexUtils::GetMachinePlatform()));
#ifdef TARGET_RASPBERRY_PI
  hdrs.push_back(stringPair("X-Plex-Device", "RaspberryPi"));
#elif defined(TARGET_DARWIN_IOS)
  hdrs.push_back(stringPair("X-Plex-Device", "AppleTV"));
#elif defined(TARGET_OPENELEC)
  hdrs.push_back(stringPair("X-Plex-Device", "OpenELEC"));
#else
  hdrs.push_back(stringPair("X-Plex-Device", "PC"));
#endif
  
  // Build a description of what we support, this is old school, but needed when
  // want PMS to select the correct audio track for us.
  CStdString protocols = "protocols=shoutcast,http-video;videoDecoders=h264{profile:high&resolution:1080&level:51};audioDecoders=mp3,aac";
  CStdString augment = "";
  
  if (g_guiSettings.GetBool("audiooutput.passthrough") || g_guiSettings.GetBool("audiooutput.supportdtshdcpudecoding"))
  {
    if (g_guiSettings.GetBool("audiooutput.dtspassthrough") || g_guiSettings.GetBool("audiooutput.ac3transcode") || g_guiSettings.GetBool("audiooutput.supportdtshdcpudecoding"))
    {
      protocols += ",dts{bitrate:800000&channels:8}";
      augment = "dca";
    }
    
    if (g_guiSettings.GetBool("audiooutput.ac3passthrough"))
    {
      protocols += ",ac3{bitrate:800000&channels:8}";
      if (augment.IsEmpty())
        augment = "ac3";
      else
        augment += ",ac3";
    }
  }
  
  hdrs.push_back(stringPair("X-Plex-Client-Capabilities", protocols));

  if (augment.IsEmpty() == false)
  {
    CStdString argstring = "add-transcode-target-audio-codec(type=videoProfile&context=streaming&protocol=*&audioCodec=" + augment + ")";
    hdrs.push_back(stringPair("X-Plex-Client-Profile-Extra", argstring));
    CLog::Log(LOGDEBUG, "PlexFile::GetHeaderList Set Client-Profile-Extra: %s", argstring.c_str());
  }
  
  if (g_plexApplication.myPlexManager && g_plexApplication.myPlexManager->IsSignedIn())
    hdrs.push_back(stringPair("X-Plex-Username", g_plexApplication.myPlexManager->GetCurrentUserInfo().username));

  hdrs.push_back(stringPair("X-Plex-Language", g_langInfo.GetLanguageLocale()));

  return hdrs;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
CPlexFile::CPlexFile(void) : CCurlFile()
{
  BOOST_FOREACH(stringPair sp, GetHeaderList())
    SetRequestHeader(sp.first, sp.second);

  SetUserAgent(PLEX_HOME_THEATER_USER_AGENT);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool CPlexFile::BuildHTTPURL(CURL& url)
{
  CURL newUrl;
  CPlexServerPtr server;

  /* allow passthrough */
  if (url.GetProtocol() != "plexserver")
    return true;

  if (!g_plexApplication.serverManager)
    return false;

  server = g_plexApplication.serverManager->FindByUUID(url.GetHostName());

  if (!server)
  {
    /* Ouch, this should not happen! */
    CLog::Log(LOGWARNING, "CPlexFile::BuildHTTPURL tried to lookup server %s but it was not found!", PlexUtils::MakeUrlSecret(url).GetHostName().c_str());
    return false;
  }

  newUrl = server->BuildURL(url.GetFileName(), url.GetOptions());
  if (newUrl.Get().empty())
    return false;

  if (url.HasProtocolOption("ssl") && url.GetProtocolOption("ssl") == "1")
  {
    newUrl.SetProtocol("https");
    if (url.GetPort() == 32400)
      newUrl.SetPort(32443);
  }

  if (!url.GetUserName().empty())
    newUrl.SetUserName(url.GetUserName());
  if (!url.GetPassWord().empty())
    newUrl.SetPassword(url.GetPassWord());

  CLog::Log(LOGDEBUG, "CPlexFile::BuildHTTPURL translated '%s' to '%s'", PlexUtils::MakeUrlSecret(url).Get().c_str(), PlexUtils::MakeUrlSecret(newUrl).Get().c_str());
  url = newUrl;

  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool CPlexFile::CanBeTranslated(const CURL &url)
{
  CURL t(url);
  return BuildHTTPURL(t);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool CPlexFile::Open(const CURL &url)
{
  CURL newUrl(url);
  if (BuildHTTPURL(newUrl))
    return CCurlFile::Open(newUrl);
  return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int CPlexFile::Stat(const CURL &url, struct __stat64 *buffer)
{
  CURL newUrl(url);

  // ImageLib makes stupid Stat() calls just to figure out
  // if this is a directory or not. Let's just shortcut that
  // and tell it everything it requests from photo/:/transcode
  // is a file
  if (newUrl.GetFileName() == "photo/:/transcode")
  {
    buffer->st_mode = _S_IFREG;
    return 0;
  }

  if (BuildHTTPURL(newUrl))
    return CCurlFile::Stat(newUrl, buffer);
  return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool CPlexFile::Exists(const CURL &url)
{
  CURL newUrl(url);
  if (BuildHTTPURL(newUrl))
    return CCurlFile::Exists(newUrl);
  return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
CStdString CPlexFile::GetMimeType(const CURL& url)
{
  /* we only handle plexserver:// stuff here */
  if (url.GetProtocol() != "plexserver")
    return "";

  CStdString path = url.GetFileName();

  if (boost::starts_with(path, "/video"))
    return "video/unknown";
  if (boost::starts_with(path, "/music"))
    return "audio/uknown";
  if (boost::starts_with(path, "/photo"))
    return "image/unknown";

  CStdString extension = URIUtils::GetExtension(path);
  return CMime::GetMimeType(extension);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int CPlexFile::IoControl(EIoControl request, void* param)
{
  if ( (request == IOCTRL_SEEK_POSSIBLE) && (boost::starts_with(m_url, ":/transcode")) )
    return 1;
  else
    return CCurlFile::IoControl(request, param);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool CPlexFile::Service(const CStdString &strURL, CStdString &strHTML)
{
  bool ret = CCurlFile::Service(strURL, strHTML);
  m_tokenInvalid = false;

  if (!ret && (m_httpresponse == 422 || m_httpresponse == 401))
  {
    if (m_httpresponse == 422)
    {
      m_tokenInvalid = true;
    }
    else
    {
      CXBMCTinyXML doc;
      doc.Parse(strHTML);
      TiXmlElement* root = doc.RootElement();
      if (root)
      {
        // do we have a <error> node?
        TiXmlNode* error = root->FirstChild("error");
        if (error && error->FirstChild())
          m_tokenInvalid = true;
      }
    }
    CLog::Log(LOGDEBUG, "CPlexFile::Service got error: %ld (token invalid: %s) from %s", m_httpresponse, m_tokenInvalid ? "YES" : "NO", strURL.c_str());

    if (m_tokenInvalid)
      g_plexApplication.myPlexManager->Poke();
  }

  return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool CPlexFile::DownloadFile(const CStdString& strURL, const CStdString& strFileName)
{
  CFile file;
  if (!file.OpenForWrite(strFileName, true))
  {
    CLog::Log(LOGWARNING, "CPlexFile::DownloadFile - Couldn't open file %s for writing", strFileName.c_str());
    return false;
  }

  if (Open(strURL))
  {
    CLog::Log(LOGDEBUG, "CPlexFile::DownloadFile - Downloading %s to %s", strURL.c_str(), strFileName.c_str());

    bool failed = false;
    int size_read = 0;
    int data_size = 0;
    uint8_t buffer[16385];
    while ((size_read = Read(buffer, sizeof(buffer) - 1)) > 0)
    {
      buffer[size_read] = 0;
      if (file.Write(buffer, size_read) != size_read)
      {
        failed = true;
        break;
      }
      data_size += size_read;
    }
    Close();
    file.Close();

    if (failed || data_size <= 0)
      CFile::Delete(strFileName);

    return !failed && data_size > 0;
  }

  CLog::Log(LOGWARNING, "CPlexFile::DownloadFile - Failed to download %s", strURL.c_str());
  return false;
}
