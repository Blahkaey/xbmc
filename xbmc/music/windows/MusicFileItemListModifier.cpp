/*
 *  Copyright (C) 2016-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "MusicFileItemListModifier.h"

#include "FileItem.h"
#include "FileItemList.h"
#include "ServiceBroker.h"
#include "filesystem/MusicDatabaseDirectory/DirectoryNode.h"
#include "guilib/LocalizeStrings.h"
#include "music/MusicDbUrl.h"
#include "music/MusicFileItemClassify.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"

#include <memory>

using namespace KODI;
using namespace XFILE::MUSICDATABASEDIRECTORY;

bool CMusicFileItemListModifier::CanModify(const CFileItemList &items) const
{
  if (MUSIC::IsMusicDb(items))
    return true;

  return false;
}

bool CMusicFileItemListModifier::Modify(CFileItemList &items) const
{
  AddQueuingFolder(items);
  return true;
}

//  Add an "* All ..." folder to the CFileItemList
//  depending on the child node
void CMusicFileItemListModifier::AddQueuingFolder(CFileItemList& items)
{
  if (!MUSIC::IsMusicDb(items))
    return;

  auto directoryNode = CDirectoryNode::ParseURL(items.GetPath());

  CFileItemPtr pItem;

  CMusicDbUrl musicUrl;
  if (!musicUrl.FromString(directoryNode->BuildPath()))
    return;

  // always show "all" items by default
  if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_MUSICLIBRARY_SHOWALLITEMS))
    return;

  // no need for "all" item when only one item
  if (items.GetObjectCount() <= 1)
    return;

  auto nodeChildType = directoryNode->GetChildType();

  // No need for "*all" when overview node and child node is "albums" or "artists"
  // without options (hence all albums or artists unfiltered).
  if (directoryNode->GetType() == NodeType::OVERVIEW &&
      (nodeChildType == NodeType::ARTIST || nodeChildType == NodeType::ALBUM) &&
      musicUrl.GetOptions().empty())
    return;
  // Smart playlist rules on parent node do not get applied to child nodes so no "*all"
  // ! @Todo: Remove this allowing "*all" once rules do get applied to child nodes.
  if (directoryNode->GetType() == NodeType::OVERVIEW &&
      (nodeChildType == NodeType::ARTIST || nodeChildType == NodeType::ALBUM) &&
      musicUrl.HasOption("xsp"))
    return;

  switch (nodeChildType)
  {
    case NodeType::ARTIST:
      pItem = std::make_shared<CFileItem>(g_localizeStrings.Get(15103)); // "All Artists"
      musicUrl.AppendPath("-1/");
      pItem->SetPath(musicUrl.ToString());
      break;

    //  All album related nodes
    case NodeType::ALBUM:
    case NodeType::ALBUM_RECENTLY_PLAYED:
    case NodeType::ALBUM_RECENTLY_ADDED:
    case NodeType::ALBUM_TOP100:
      pItem = std::make_shared<CFileItem>(g_localizeStrings.Get(15102)); // "All Albums"
      musicUrl.AppendPath("-1/");
      pItem->SetPath(musicUrl.ToString());
      break;

    //  Disc node
    case NodeType::DISC:
      pItem = std::make_shared<CFileItem>(g_localizeStrings.Get(38075)); // "All Discs"
      musicUrl.AppendPath("-1/");
      pItem->SetPath(musicUrl.ToString());
      break;

    default:
      break;
  }

  if (pItem)
  {
    pItem->SetFolder(true);
    pItem->SetSpecialSort(CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_bMusicLibraryAllItemsOnBottom ? SortSpecialOnBottom : SortSpecialOnTop);
    pItem->SetCanQueue(false);
    pItem->SetLabelPreformatted(true);
    if (CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_bMusicLibraryAllItemsOnBottom)
      items.Add(pItem);
    else
      items.AddFront(pItem, (items.Size() > 0 && items[0]->IsParentFolder()) ? 1 : 0);
  }
}
