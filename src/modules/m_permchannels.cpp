/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2009-2010 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2008-2009 Robin Burchell <robin+git@viroteck.net>
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
#include <fstream>


/** Handles the +P channel mode
 */
class PermChannel : public ModeHandler
{
 public:
	PermChannel(Module* Creator)
		: ModeHandler(Creator, "permanent", 'P', PARAM_NONE, MODETYPE_CHANNEL)
	{
		oper = true;
	}

	ModeAction OnModeChange(User* source, User* dest, Channel* channel, std::string& parameter, bool adding)
	{
		if (adding == channel->IsModeSet(this))
			return MODEACTION_DENY;

		channel->SetMode(this, adding);
		if (!adding)
			channel->CheckDestroy();

		return MODEACTION_ALLOW;
	}
};

// Not in a class due to circular dependancy hell.
static std::string permchannelsconf;
static bool WriteDatabase(PermChannel& permchanmode)
{
	/*
	 * We need to perform an atomic write so as not to fuck things up.
	 * So, let's write to a temporary file, flush it, then rename the file..
	 *     -- w00t
	 */
	
	// If the user has not specified a configuration file then we don't write one.
	if (permchannelsconf.empty())
		return true;

	std::string permchannelsnewconf = permchannelsconf + ".tmp";
	std::ofstream stream(permchannelsnewconf.c_str());
	if (!stream.is_open())
	{
		ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, "Cannot create database! %s (%d)", strerror(errno), errno);
		ServerInstance->SNO->WriteToSnoMask('a', "database: cannot create new db: %s (%d)", strerror(errno), errno);
		return false;
	}
	
	stream << "# This file is automatically generated by m_permchannels. Any changes will be overwritten." << std::endl
		<< "<config format=\"xml\">" << std::endl;

	for (chan_hash::const_iterator i = ServerInstance->chanlist->begin(); i != ServerInstance->chanlist->end(); i++)
	{
		Channel* chan = i->second;
		if (!chan->IsModeSet(permchanmode))
			continue;

		stream << "<permchannels channel=\"" << ServerConfig::Escape(chan->name)
			<< "\" topic=\"" << ServerConfig::Escape(chan->topic)
			<< "\" modes=\"" << ServerConfig::Escape(chan->ChanModes(true))
			<< "\">" << std::endl;
	}

	if (stream.fail())
	{
		ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, "Cannot write to new database! %s (%d)", strerror(errno), errno);
		ServerInstance->SNO->WriteToSnoMask('a', "database: cannot write to new db: %s (%d)", strerror(errno), errno);
		return false;
	}
	stream.close();

#ifdef _WIN32
	if (remove(permchannelsconf.c_str()))
	{
		ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, "Cannot remove old database! %s (%d)", strerror(errno), errno);
		ServerInstance->SNO->WriteToSnoMask('a', "database: cannot remove old database: %s (%d)", strerror(errno), errno);
		return false;
	}
#endif
	// Use rename to move temporary to new db - this is guarenteed not to fuck up, even in case of a crash.
	if (rename(permchannelsnewconf.c_str(), permchannelsconf.c_str()) < 0)
	{
		ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, "Cannot move new to old database! %s (%d)", strerror(errno), errno);
		ServerInstance->SNO->WriteToSnoMask('a', "database: cannot replace old with new db: %s (%d)", strerror(errno), errno);
		return false;
	}

	return true;
}

class ModulePermanentChannels : public Module
{
	PermChannel p;
	bool dirty;
public:

	ModulePermanentChannels() : p(this), dirty(false)
	{
	}

	void init() CXX11_OVERRIDE
	{
		ServerInstance->Modules->AddService(p);
		Implementation eventlist[] = { I_OnChannelPreDelete, I_OnPostTopicChange, I_OnRawMode, I_OnRehash, I_OnBackgroundTimer };
		ServerInstance->Modules->Attach(eventlist, this, sizeof(eventlist)/sizeof(Implementation));

		OnRehash(NULL);
	}

	CullResult cull()
	{
		/*
		 * DelMode can't remove the +P mode on empty channels, or it will break
		 * merging modes with remote servers. Remove the empty channels now as
		 * we know this is not the case.
		 */
		chan_hash::iterator iter = ServerInstance->chanlist->begin();
		while (iter != ServerInstance->chanlist->end())
		{
			Channel* c = iter->second;
			if (c->GetUserCounter() == 0)
			{
				chan_hash::iterator at = iter;
				iter++;
				FOREACH_MOD(I_OnChannelDelete, OnChannelDelete(c));
				ServerInstance->chanlist->erase(at);
				ServerInstance->GlobalCulls.AddItem(c);
			}
			else
				iter++;
		}
		ServerInstance->Modes->DelMode(&p);
		return Module::cull();
	}

	void OnRehash(User *user) CXX11_OVERRIDE
	{
		permchannelsconf = ServerInstance->Config->ConfValue("permchanneldb")->getString("filename");
	}

	void LoadDatabase()
	{
		/*
		 * Process config-defined list of permanent channels.
		 * -- w00t
		 */
		ConfigTagList permchannels = ServerInstance->Config->ConfTags("permchannels");
		for (ConfigIter i = permchannels.first; i != permchannels.second; ++i)
		{
			ConfigTag* tag = i->second;
			std::string channel = tag->getString("channel");
			std::string topic = tag->getString("topic");
			std::string modes = tag->getString("modes");

			if (channel.empty())
			{
				ServerInstance->Logs->Log(MODNAME, LOG_DEBUG, "Malformed permchannels tag with empty channel name.");
				continue;
			}

			Channel *c = ServerInstance->FindChan(channel);

			if (!c)
			{
				c = new Channel(channel, ServerInstance->Time());
				if (!topic.empty())
				{
					c->SetTopic(ServerInstance->FakeClient, topic);

					/*
					 * Due to the way protocol works in 1.2, we need to hack the topic TS in such a way that this
					 * topic will always win over others.
					 *
					 * This is scheduled for (proper) fixing in a later release, and can be removed at a later date.
					 */
					c->topicset = 42;
				}
				ServerInstance->Logs->Log(MODNAME, LOG_DEBUG, "Added %s with topic %s", channel.c_str(), topic.c_str());

				if (modes.empty())
					continue;

				irc::spacesepstream list(modes);
				std::string modeseq;
				std::string par;

				list.GetToken(modeseq);

				// XXX bleh, should we pass this to the mode parser instead? ugly. --w00t
				for (std::string::iterator n = modeseq.begin(); n != modeseq.end(); ++n)
				{
					ModeHandler* mode = ServerInstance->Modes->FindMode(*n, MODETYPE_CHANNEL);
					if (mode)
					{
						if (mode->GetNumParams(true))
							list.GetToken(par);
						else
							par.clear();

						mode->OnModeChange(ServerInstance->FakeClient, ServerInstance->FakeClient, c, par, true);
					}
				}
			}
		}
	}

	ModResult OnRawMode(User* user, Channel* chan, const char mode, const std::string &param, bool adding, int pcnt) CXX11_OVERRIDE
	{
		if (chan && (chan->IsModeSet(p) || mode == p.GetModeChar()))
			dirty = true;

		return MOD_RES_PASSTHRU;
	}

	void OnPostTopicChange(User*, Channel *c, const std::string&) CXX11_OVERRIDE
	{
		if (c->IsModeSet(p))
			dirty = true;
	}

	void OnBackgroundTimer(time_t) CXX11_OVERRIDE
	{
		if (dirty)
			WriteDatabase(p);
		dirty = false;
	}

	void Prioritize()
	{
		// XXX: Load the DB here because the order in which modules are init()ed at boot is
		// alphabetical, this means we must wait until all modules have done their init()
		// to be able to set the modes they provide (e.g.: m_stripcolor is inited after us)
		// Prioritize() is called after all module initialization is complete, consequently
		// all modes are available now

		static bool loaded = false;
		if (loaded)
			return;

		loaded = true;

		// Load only when there are no linked servers - we set the TS of the channels we
		// create to the current time, this can lead to desync because spanningtree has
		// no way of knowing what we do
		ProtoServerList serverlist;
		ServerInstance->PI->GetServerList(serverlist);
		if (serverlist.size() < 2)
		{
			try
			{
				LoadDatabase();
			}
			catch (CoreException& e)
			{
				ServerInstance->Logs->Log(MODNAME, LOG_DEFAULT, "Error loading permchannels database: " + std::string(e.GetReason()));
			}
		}
	}

	Version GetVersion() CXX11_OVERRIDE
	{
		return Version("Provides support for channel mode +P to provide permanent channels",VF_VENDOR);
	}

	ModResult OnChannelPreDelete(Channel *c) CXX11_OVERRIDE
	{
		if (c->IsModeSet(p))
			return MOD_RES_DENY;

		return MOD_RES_PASSTHRU;
	}
};

MODULE_INIT(ModulePermanentChannels)
