/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

////////////////////////////////////////////////////////////////////////////////////
// Class: CueDocument
// This class handles the .cue file format.  This is produced by programs such as
// EAC and CDRwin when one extracts audio data from a CD as a continuous .WAV
// containing all the audio tracks in one big file.  The .cue file contains all the
// track and timing information.  An example file is:
//
// PERFORMER "Pink Floyd"
// TITLE "The Dark Side Of The Moon"
// FILE "The Dark Side Of The Moon.mp3" WAVE
//   TRACK 01 AUDIO
//     TITLE "Speak To Me / Breathe"
//     PERFORMER "Pink Floyd"
//     INDEX 00 00:00:00
//     INDEX 01 00:00:32
//   TRACK 02 AUDIO
//     TITLE "On The Run"
//     PERFORMER "Pink Floyd"
//     INDEX 00 03:58:72
//     INDEX 01 04:00:72
//   TRACK 03 AUDIO
//     TITLE "Time"
//     PERFORMER "Pink Floyd"
//     INDEX 00 07:31:70
//     INDEX 01 07:33:70
//
// etc.
//
// The CCueDocument class member functions extract this information, and construct
// the playlist items needed to seek to a track directly.  This works best on CBR
// compressed files - VBR files do not seek accurately enough for it to work well.
//
////////////////////////////////////////////////////////////////////////////////////

#include "CueDocument.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "ServiceBroker.h"
#include "Util.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "music/tags/MusicInfoTag.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/CharsetConverter.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cstdlib>
#include <set>

using namespace XFILE;

// Stuff for read CUE data from different sources.
class CueReader
{
public:
  virtual bool ready() const = 0;
  virtual bool ReadLine(std::string &line) = 0;
  virtual ~CueReader() = default;
};

class FileReader : public CueReader
{
public:
  explicit FileReader(const std::string& strFile) { m_opened = m_file.Open(strFile); }

  bool ReadLine(std::string &line) override
  {
    // Read the next line.
    while (m_file.ReadLine(line))
    {
      // Remove the white space at the beginning and end of the line.
      StringUtils::Trim(line);
      if (!line.empty())
        return true;
      // If we are here, we have an empty line so try the next line
    }
    return false;
  }

  bool ready() const override
  {
    return m_opened;
  }

  ~FileReader() override
  {
    if (m_opened)
      m_file.Close();
  }

private:
  CFile m_file;
  bool m_opened;
};

class BufferReader : public CueReader
{
public:
  explicit BufferReader(const std::string& strContent) : m_data(strContent) {}

  bool ReadLine(std::string &line) override
  {
    // Read the next line.
    line.clear();
    while (m_pos < m_data.size())
    {
      // Remove the white space at the beginning of the line.
      char ch = m_data.at(m_pos++);
      if (ch == '\r' || ch == '\n') {
        StringUtils::Trim(line);
        if (!line.empty())
          return true;
      }
      else
      {
        line.push_back(ch);
      }
    }

    StringUtils::Trim(line);
    return !line.empty();
  }

  bool ready() const override { return !m_data.empty(); }

private:
  std::string m_data;
  size_t m_pos = 0;
};

CCueDocument::~CCueDocument() = default;

////////////////////////////////////////////////////////////////////////////////////
// Function: ParseFile()
// Opens the CUE file for reading, and constructs the track database information
////////////////////////////////////////////////////////////////////////////////////
bool CCueDocument::ParseFile(const std::string &strFilePath)
{
  FileReader reader(strFilePath);
  return Parse(reader, strFilePath);
}

////////////////////////////////////////////////////////////////////////////////////
// Function: ParseTag()
// Reads CUE data from string buffer, and constructs the track database information
////////////////////////////////////////////////////////////////////////////////////
bool CCueDocument::ParseTag(const std::string &strContent)
{
  BufferReader reader(strContent);
  return Parse(reader);
}

//////////////////////////////////////////////////////////////////////////////////
// Function:GetSongs()
// Store track information into songs list.
//////////////////////////////////////////////////////////////////////////////////
void CCueDocument::GetSongs(VECSONGS &songs)
{
  const auto separator =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_musicItemSeparator;

  std::ranges::transform(
      m_tracks, std::back_inserter(songs),
      [&artist = m_strArtist, &album = m_strAlbum, &replayGain = m_albumReplayGain,
       disc = m_iDiscNumber, year = StringUtils::Format("{:04}", m_iYear),
       artists = StringUtils::Split(m_strArtist, separator),
       genres = StringUtils::Split(m_strGenre, separator)](const auto& track)
      {
        CSong aSong;
        //Pass artist to MusicInfoTag object by setting artist description string only.
        //Artist credits not used during loading from cue sheet.
        if (track.strArtist.empty() && !artist.empty())
          aSong.strArtistDesc = artist;
        else
          aSong.strArtistDesc = track.strArtist;
        //Pass album artist to MusicInfoTag object by setting album artist vector.
        aSong.SetAlbumArtist(artists);
        aSong.strAlbum = album;
        aSong.genre = genres;
        aSong.strReleaseDate = year;
        aSong.iTrack = track.iTrackNumber;
        if (disc > 0)
          aSong.iTrack |= disc << 16; // see CMusicInfoTag::GetDiscNumber()
        if (track.strTitle.length() == 0) // No track information for this track!
          aSong.strTitle = StringUtils::Format("Track {:2d}", track.iTrackNumber);
        else
          aSong.strTitle = track.strTitle;
        aSong.strFileName = track.strFile;
        aSong.iStartOffset = track.iStartTime;
        aSong.iEndOffset = track.iEndTime;
        if (aSong.iEndOffset)
          // Convert offset in frames (75 per second) to duration in whole seconds with rounding
          aSong.iDuration =
              CUtil::ConvertMilliSecsToSecsIntRounded(aSong.iEndOffset - aSong.iStartOffset);
        else
          aSong.iDuration = 0;

        if (replayGain.Valid())
          aSong.replayGain.Set(ReplayGain::ALBUM, replayGain);

        if (track.replayGain.Valid())
          aSong.replayGain.Set(ReplayGain::TRACK, track.replayGain);

        return aSong;
      });
}

void CCueDocument::UpdateMediaFile(const std::string& oldMediaFile, const std::string& mediaFile)
{
  for (auto& track : m_tracks)
  {
    if (track.strFile == oldMediaFile)
      track.strFile = mediaFile;
  }
}

void CCueDocument::GetMediaFiles(std::vector<std::string>& mediaFiles)
{
  std::set<std::string> uniqueFiles;
  std::ranges::transform(m_tracks, std::inserter(uniqueFiles, uniqueFiles.end()),
                         [](const auto& track) { return track.strFile; });

  std::ranges::copy(uniqueFiles, std::back_inserter(mediaFiles));
}

bool CCueDocument::IsLoaded() const
{
  return !m_tracks.empty();
}

bool CCueDocument::IsOneFilePerTrack() const
{
  return m_bOneFilePerTrack;
}

// Private Functions start here

void CCueDocument::Clear()
{
  m_strArtist.clear();
  m_strAlbum.clear();
  m_strGenre.clear();
  m_iYear = 0;
  m_iTrack = 0;
  m_iDiscNumber = 0;
  m_albumReplayGain = ReplayGain::Info();
  m_tracks.clear();
}
////////////////////////////////////////////////////////////////////////////////////
// Function: Parse()
// Constructs the track database information from CUE source
////////////////////////////////////////////////////////////////////////////////////
bool CCueDocument::Parse(CueReader& reader, const std::string& strFile)
{
  Clear();
  if (!reader.ready())
    return false;

  std::string strLine;
  std::string strCurrentFile = "";
  bool bCurrentFileChanged = false;
  int time;
  int totalTracks = -1;
  int numberFiles = -1;

  // Run through the .CUE file and extract the tracks...
  while (reader.ReadLine(strLine))
  {
    if (StringUtils::StartsWithNoCase(strLine, "INDEX 01"))
    {
      if (bCurrentFileChanged)
      {
        CLog::Log(LOGERROR, "Track split over multiple files, unsupported.");
        return false;
      }

      // find the end of the number section
      time = ExtractTimeFromIndex(strLine);
      if (time == -1)
      { // Error!
        CLog::Log(LOGERROR, "Mangled Time in INDEX 0x tag in CUE file!");
        return false;
      }
      if (totalTracks > 0 && m_tracks[totalTracks - 1].strFile == strCurrentFile) // Set the end time of the last track
        m_tracks[totalTracks - 1].iEndTime = time;

      if (totalTracks >= 0) // start time of the next track
        m_tracks[totalTracks].iStartTime = time;
    }
    else if (StringUtils::StartsWithNoCase(strLine, "TITLE"))
    {
      if (totalTracks == -1) // No tracks yet
        m_strAlbum = ExtractInfo(strLine.substr(5));
      else
        m_tracks[totalTracks].strTitle = ExtractInfo(strLine.substr(5));
    }
    else if (StringUtils::StartsWithNoCase(strLine, "PERFORMER"))
    {
      if (totalTracks == -1) // No tracks yet
        m_strArtist = ExtractInfo(strLine.substr(9));
      else // New Artist for this track
        m_tracks[totalTracks].strArtist = ExtractInfo(strLine.substr(9));
    }
    else if (StringUtils::StartsWithNoCase(strLine, "TRACK"))
    {
      int iTrackNumber = ExtractNumericInfo(strLine.substr(5));

      totalTracks++;

      CCueTrack track;
      m_tracks.push_back(track);
      m_tracks[totalTracks].strFile = strCurrentFile;
      if (iTrackNumber > 0)
        m_tracks[totalTracks].iTrackNumber = iTrackNumber;
      else
        m_tracks[totalTracks].iTrackNumber = totalTracks + 1;

      bCurrentFileChanged = false;
    }
    else if (StringUtils::StartsWithNoCase(strLine, "REM DISCNUMBER"))
    {
      int iDiscNumber = ExtractNumericInfo(strLine.substr(14));
      if (iDiscNumber > 0)
        m_iDiscNumber = iDiscNumber;
    }
    else if (StringUtils::StartsWithNoCase(strLine, "FILE"))
    {
      numberFiles++;
      // already a file name? then the time computation will be changed
      if (!strCurrentFile.empty())
        bCurrentFileChanged = true;

      strCurrentFile = ExtractInfo(strLine.substr(4));

      // Resolve absolute paths (if needed).
      if (!strFile.empty() && !strCurrentFile.empty())
        ResolvePath(strCurrentFile, strFile);
    }
    else if (StringUtils::StartsWithNoCase(strLine, "REM DATE"))
    {
      int iYear = ExtractNumericInfo(strLine.substr(8));
      if (iYear > 0)
        m_iYear = iYear;
    }
    else if (StringUtils::StartsWithNoCase(strLine, "REM GENRE"))
    {
      m_strGenre = ExtractInfo(strLine.substr(9));
    }
    else if (StringUtils::StartsWithNoCase(strLine, "REM REPLAYGAIN_ALBUM_GAIN"))
      m_albumReplayGain.SetGain(strLine.substr(26));
    else if (StringUtils::StartsWithNoCase(strLine, "REM REPLAYGAIN_ALBUM_PEAK"))
      m_albumReplayGain.SetPeak(strLine.substr(26));
    else if (StringUtils::StartsWithNoCase(strLine, "REM REPLAYGAIN_TRACK_GAIN") && totalTracks >= 0)
      m_tracks[totalTracks].replayGain.SetGain(strLine.substr(26));
    else if (StringUtils::StartsWithNoCase(strLine, "REM REPLAYGAIN_TRACK_PEAK") && totalTracks >= 0)
      m_tracks[totalTracks].replayGain.SetPeak(strLine.substr(26));
  }

  // reset track counter to 0, and fill in the last tracks end time
  m_iTrack = 0;
  if (totalTracks >= 0)
    m_tracks[totalTracks].iEndTime = 0;
  else
    CLog::Log(LOGERROR, "No INDEX 01 tags in CUE file!");

  if ( totalTracks == numberFiles )
    m_bOneFilePerTrack = true;

  return (totalTracks >= 0);
}

////////////////////////////////////////////////////////////////////////////////////
// Function: ExtractInfo()
// Extracts the information in quotes from the string line, returning it in quote
////////////////////////////////////////////////////////////////////////////////////
std::string CCueDocument::ExtractInfo(const std::string &line)
{
  size_t left = line.find('\"');
  if (left != std::string::npos)
  {
    size_t right = line.find('\"', left + 1);
    if (right != std::string::npos)
    {
      std::string text = line.substr(left + 1, right - left - 1);
      g_charsetConverter.unknownToUTF8(text);
      return text;
    }
  }
  std::string text = line;
  StringUtils::Trim(text);
  g_charsetConverter.unknownToUTF8(text);
  return text;
}

////////////////////////////////////////////////////////////////////////////////////
// Function: ExtractTimeFromIndex()
// Extracts the time information from the index string index, returning it as a value in
// milliseconds.
// Assumed format is:
// MM:SS:FF where MM is minutes, SS seconds, and FF frames (75 frames in a second)
////////////////////////////////////////////////////////////////////////////////////
int CCueDocument::ExtractTimeFromIndex(const std::string &index)
{
  // Get rid of the index number and any whitespace
  std::string numberTime = index.substr(5);
  StringUtils::TrimLeft(numberTime);
  while (!numberTime.empty())
  {
    if (!StringUtils::isasciidigit(numberTime[0]))
      break;
    numberTime.erase(0, 1);
  }
  StringUtils::TrimLeft(numberTime);
  // split the resulting string
  std::vector<std::string> time = StringUtils::Split(numberTime, ":");
  if (time.size() != 3)
    return -1;

  int mins = atoi(time[0].c_str());
  int secs = atoi(time[1].c_str());
  int frames = atoi(time[2].c_str());

  return CUtil::ConvertSecsToMilliSecs(mins*60 + secs) + frames * 1000 / 75;
}

////////////////////////////////////////////////////////////////////////////////////
// Function: ExtractNumericInfo()
// Extracts the numeric info from the string info, returning it as an integer value
////////////////////////////////////////////////////////////////////////////////////
int CCueDocument::ExtractNumericInfo(const std::string &info)
{
  std::string number(info);
  StringUtils::TrimLeft(number);
  if (number.empty() || !StringUtils::isasciidigit(number[0]))
    return -1;
  return atoi(number.c_str());
}

////////////////////////////////////////////////////////////////////////////////////
// Function: ResolvePath()
// Determines whether strPath is a relative path or not, and if so, converts it to an
// absolute path using the path information in strBase
////////////////////////////////////////////////////////////////////////////////////
bool CCueDocument::ResolvePath(std::string &strPath, const std::string &strBase)
{
  std::string strDirectory = URIUtils::GetDirectory(strBase);
  std::string strFilename = URIUtils::GetFileName(strPath);

  strPath = URIUtils::AddFileToFolder(strDirectory, strFilename);

  if (!CFile::Exists(strPath))
  {
    CFileItemList items;
    CDirectory::GetDirectory(strDirectory, items, "", DIR_FLAG_DEFAULTS);
    const auto it =
        std::ranges::find_if(items, [&strPath](const auto& item) { return item->IsPath(strPath); });
    if (it == items.end())
      CLog::Log(LOGERROR, "Could not find '{}' referenced in cue, case sensitivity issue?",
                strPath);
    return it != items.end();
  }

  return true;
}

bool CCueDocument::LoadTracks(CFileItemList& scannedItems, const CFileItem& item)
{
  const auto& tag = *item.GetMusicInfoTag();

  VECSONGS tracks;
  this->GetSongs(tracks);

  bool oneFilePerTrack = this->IsOneFilePerTrack();

  int tracksFound = 0;
  for (auto& song : tracks)
  {
    if (song.strFileName == item.GetPath())
    {
      if (tag.Loaded())
      {
        if (song.strAlbum.empty() && !tag.GetAlbum().empty())
          song.strAlbum = tag.GetAlbum();
        //Pass album artist to final MusicInfoTag object via setting song album artist vector.
        if (song.GetAlbumArtist().empty() && !tag.GetAlbumArtist().empty())
          song.SetAlbumArtist(tag.GetAlbumArtist());
        if (song.genre.empty() && !tag.GetGenre().empty())
          song.genre = tag.GetGenre();
        //Pass artist to final MusicInfoTag object via setting song artist description string only.
        //Artist credits not used during loading from cue sheet.
        if (song.strArtistDesc.empty() && !tag.GetArtistString().empty())
          song.strArtistDesc = tag.GetArtistString();
        if (tag.GetDiscNumber())
          song.iTrack |= (tag.GetDiscNumber() << 16); // see CMusicInfoTag::GetDiscNumber()
        if (!tag.GetCueSheet().empty())
          song.strCueSheet = tag.GetCueSheet();

        if (tag.GetYear())
          song.strReleaseDate = tag.GetReleaseDate();
        if (song.embeddedArt.Empty() && !tag.GetCoverArtInfo().Empty())
          song.embeddedArt = tag.GetCoverArtInfo();
      }

      if (!song.iDuration && tag.GetDuration() > 0)
      { // must be the last song
        song.iDuration = CUtil::ConvertMilliSecsToSecsIntRounded(
            CUtil::ConvertSecsToMilliSecs(tag.GetDuration()) - song.iStartOffset);
      }
      if (tag.Loaded() && oneFilePerTrack &&
          !(tag.GetAlbum().empty() || tag.GetArtist().empty() || tag.GetTitle().empty()))
      {
        // If there are multiple files in a cue file, the tags from the files should be preferred if they exist.
        scannedItems.Add(std::make_shared<CFileItem>(song, tag));
      }
      else
      {
        scannedItems.Add(std::make_shared<CFileItem>(song));
      }
      ++tracksFound;
    }
  }
  return tracksFound != 0;
}
