/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2018-2019 Sadie Powell <sadie@witchery.services>
 *   Copyright (C) 2013 Attila Molnar <attilamolnar@hush.com>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "inspircd.h"

#include "main.h"
#include "commands.h"
#include "treeserver.h"

void CmdBuilder::FireEvent(Server* target, const char* cmd, ClientProtocol::TagMap& taglist)
{
	Utils->Creator->GetMessageEventProvider().Call(&ServerProtocol::MessageEventListener::OnBuildServerMessage, target, cmd, taglist);
	UpdateTags();
}

void CmdBuilder::FireEvent(User* target, const char* cmd, ClientProtocol::TagMap& taglist)
{
	Utils->Creator->GetMessageEventProvider().Call(&ServerProtocol::MessageEventListener::OnBuildUserMessage, target, cmd, taglist);
	UpdateTags();
}

void CmdBuilder::UpdateTags()
{
	std::string taglist;
	if (!tags.empty())
	{
		char separator = '@';
		for (ClientProtocol::TagMap::const_iterator iter = tags.begin(); iter != tags.end(); ++iter)
		{
			taglist.push_back(separator);
			separator = ';';
			taglist.append(iter->first);
			if (!iter->second.value.empty())
			{
				taglist.push_back('=');
				taglist.append(iter->second.value);
			}
		}
		taglist.push_back(' ');
	}
	content.replace(0, tagsize, taglist);
	tagsize = taglist.length();
}

CmdResult CommandSNONotice::Handle(User* user, Params& params)
{
	ServerInstance->SNO.WriteToSnoMask(params[0][0], "From " + user->nick + ": " + params[1]);
	return CmdResult::SUCCESS;
}

CmdResult CommandEndBurst::HandleServer(TreeServer* server, Params& params)
{
	server->FinishBurst();
	return CmdResult::SUCCESS;
}
