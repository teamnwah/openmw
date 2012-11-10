
#include "dialoguemanagerimp.hpp"

#include <cctype>
#include <algorithm>
#include <iterator>

#include <components/esm/loaddial.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/scriptmanager.hpp"
#include "../mwbase/journal.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"

#include "../mwgui/dialogue.hpp"

#include <iostream>

#include <components/compiler/exception.hpp>
#include <components/compiler/errorhandler.hpp>
#include <components/compiler/scanner.hpp>
#include <components/compiler/locals.hpp>
#include <components/compiler/output.hpp>
#include <components/compiler/scriptparser.hpp>
#include <components/interpreter/interpreter.hpp>

#include "../mwscript/compilercontext.hpp"
#include "../mwscript/interpretercontext.hpp"
#include "../mwscript/extensions.hpp"

#include "../mwclass/npc.hpp"
#include "../mwmechanics/npcstats.hpp"
#include "../mwmechanics/creaturestats.hpp"

#include "filter.hpp"

namespace
{
    std::string toLower (const std::string& name)
    {
        std::string lowerCase;

        std::transform (name.begin(), name.end(), std::back_inserter (lowerCase),
            (int(*)(int)) std::tolower);

        return lowerCase;
    }

    bool stringCompareNoCase (std::string first, std::string second)
    {
        unsigned int i=0;
        while ( (i<first.length()) && (i<second.length()) )
        {
            if (tolower(first[i])<tolower(second[i])) return true;
            else if (tolower(first[i])>tolower(second[i])) return false;
            ++i;
        }
        if (first.length()<second.length())
            return true;
        else
            return false;
    }

    template<typename T1, typename T2>
    bool selectCompare (char comp, T1 value1, T2 value2)
    {
        switch (comp)
        {
        case '0': return value1==value2;
        case '1': return value1!=value2;
        case '2': return value1>value2;
        case '3': return value1>=value2;
        case '4': return value1<value2;
        case '5': return value1<=value2;
        }

        throw std::runtime_error ("unknown compare type in dialogue info select");
    }

    //helper function
    std::string::size_type find_str_ci(const std::string& str, const std::string& substr,size_t pos)
    {
        return toLower(str).find(toLower(substr),pos);
    }
}

namespace MWDialogue
{


    bool DialogueManager::functionFilter(const MWWorld::Ptr& actor, const ESM::DialInfo& info,bool choice)
    {
        for (std::vector<ESM::DialInfo::SelectStruct>::const_iterator iter (info.mSelects.begin());
            iter != info.mSelects.end(); ++iter)
        {
            ESM::DialInfo::SelectStruct select = *iter;
            char type = select.mSelectRule[1];
            if(type == '1')
            {
                char comp = select.mSelectRule[4];
                std::string name = select.mSelectRule.substr (5);
                std::string function = select.mSelectRule.substr(2,2);

                int ifunction;
                std::istringstream iss(function);
                iss >> ifunction;
                switch(ifunction)
                {
                case 48://Detected
                    if(!selectCompare<int,int>(comp,1,select.mI)) return false;
                    break;

                case 49://Alarmed
                    if(!selectCompare<int,int>(comp,0,select.mI)) return false;
                    break;

                case 61://Level
                    if(!selectCompare<int,int>(comp,1,select.mI)) return false;
                    break;

                case 62://Attacked
                    if(!selectCompare<int,int>(comp,0,select.mI)) return false;
                    break;

                case 65://Creature target
                    if(!selectCompare<int,int>(comp,0,select.mI)) return false;
                    break;

                case 71://Should Attack
                    if(!selectCompare<int,int>(comp,0,select.mI)) return false;
                    break;

                default:
                    break;

                }
            }
        }

        return true;
    }

    bool DialogueManager::isMatching (const MWWorld::Ptr& actor,
        const ESM::DialInfo::SelectStruct& select) const
    {
        return true;
    }

    bool DialogueManager::isMatching (const MWWorld::Ptr& actor, const ESM::DialInfo& info) const
    {
        // check DATAstruct
        for (std::vector<ESM::DialInfo::SelectStruct>::const_iterator iter (info.mSelects.begin());
            iter != info.mSelects.end(); ++iter)
            if (!isMatching (actor, *iter))
                return false;

        return true;
    }

    DialogueManager::DialogueManager (const Compiler::Extensions& extensions) :
      mCompilerContext (MWScript::CompilerContext::Type_Dialgoue),
        mErrorStream(std::cout.rdbuf()),mErrorHandler(mErrorStream)
    {
        mChoice = -1;
        mIsInChoice = false;
        mCompilerContext.setExtensions (&extensions);
        mDialogueMap.clear();
        mActorKnownTopics.clear();

        const MWWorld::Store<ESM::Dialogue> &dialogs =
            MWBase::Environment::get().getWorld()->getStore().get<ESM::Dialogue>();

        MWWorld::Store<ESM::Dialogue>::iterator it = dialogs.begin();
        for (; it != dialogs.end(); ++it)
        {
            mDialogueMap[toLower(it->mId)] = *it;
        }
    }

    void DialogueManager::addTopic (const std::string& topic)
    {
        mKnownTopics[toLower(topic)] = true;
    }

    void DialogueManager::parseText (std::string text)
    {
        std::list<std::string>::iterator it;
        for(it = mActorKnownTopics.begin();it != mActorKnownTopics.end();++it)
        {
            size_t pos = find_str_ci(text,*it,0);
            if(pos !=std::string::npos)
            {
                if(pos==0)
                {
                    mKnownTopics[*it] = true;
                }
                else if(text.substr(pos -1,1) == " ")
                {
                    mKnownTopics[*it] = true;
                }
            }
        }
        updateTopics();
    }

    void DialogueManager::startDialogue (const MWWorld::Ptr& actor)
    {
        mChoice = -1;
        mIsInChoice = false;

        mActor = actor;
        
        MWMechanics::CreatureStats& creatureStats = MWWorld::Class::get (actor).getCreatureStats (actor);
        mTalkedTo = creatureStats.hasTalkedToPlayer();
        creatureStats.talkedToPlayer();

        mActorKnownTopics.clear();

        //initialise the GUI
        MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_Dialogue);
        MWGui::DialogueWindow* win = MWBase::Environment::get().getWindowManager()->getDialogueWindow();
        win->startDialogue(actor, MWWorld::Class::get (actor).getName (actor));

        //setup the list of topics known by the actor. Topics who are also on the knownTopics list will be added to the GUI
        updateTopics();

        //greeting
        bool greetingFound = false;
        const MWWorld::Store<ESM::Dialogue> &dialogs =
            MWBase::Environment::get().getWorld()->getStore().get<ESM::Dialogue>();

        Filter filter (actor, mChoice, mTalkedTo);

        MWWorld::Store<ESM::Dialogue>::iterator it = dialogs.begin();
        for (; it != dialogs.end(); ++it)
        {
            if(it->mType == ESM::Dialogue::Greeting)
            {
                if (greetingFound) break;
                for (std::vector<ESM::DialInfo>::const_iterator iter (it->mInfo.begin());
                    iter!=it->mInfo.end(); ++iter)
                {
                    if (filter (*iter) && isMatching (actor, *iter) && functionFilter(mActor,*iter,true))
                    {
                        if (!iter->mSound.empty())
                        {
                            // TODO play sound
                        }

                        std::string text = iter->mResponse;
                        parseText(text);
                        win->addText(iter->mResponse);
                        executeScript(iter->mResultScript);
                        greetingFound = true;
                        mLastTopic = it->mId;
                        mLastDialogue = *iter;
                        break;
                    }
                }
            }
        }
    }

    bool DialogueManager::compile (const std::string& cmd,std::vector<Interpreter::Type_Code>& code)
    {
        try
        {
            mErrorHandler.reset();

            std::istringstream input (cmd + "\n");

            Compiler::Scanner scanner (mErrorHandler, input, mCompilerContext.getExtensions());

            Compiler::Locals locals;

            std::string actorScript = MWWorld::Class::get (mActor).getScript (mActor);

            if (!actorScript.empty())
            {
                // grab local variables from actor's script, if available.
                locals = MWBase::Environment::get().getScriptManager()->getLocals (actorScript);
            }

            Compiler::ScriptParser parser(mErrorHandler,mCompilerContext, locals, false);

            scanner.scan (parser);
            if(mErrorHandler.isGood())
            {
                parser.getCode(code);
                return true;
            }
            return false;
        }
        catch (const Compiler::SourceException& /* error */)
        {
            // error has already been reported via error handler
        }
        catch (const std::exception& error)
        {
            printError (std::string ("An exception has been thrown: ") + error.what());
        }

        return false;
    }

    void DialogueManager::executeScript(std::string script)
    {
        std::vector<Interpreter::Type_Code> code;
        if(compile(script,code))
        {
            try
            {
                MWScript::InterpreterContext interpreterContext(&mActor.getRefData().getLocals(),mActor);
                Interpreter::Interpreter interpreter;
                MWScript::installOpcodes (interpreter);
                interpreter.run (&code[0], code.size(), interpreterContext);
            }
            catch (const std::exception& error)
            {
                printError (std::string ("An exception has been thrown: ") + error.what());
            }
        }
    }

    void DialogueManager::updateTopics()
    {
        std::list<std::string> keywordList;
        int choice = mChoice;
        mChoice = -1;
        mActorKnownTopics.clear();
        MWGui::DialogueWindow* win = MWBase::Environment::get().getWindowManager()->getDialogueWindow();

        const MWWorld::Store<ESM::Dialogue> &dialogs =
            MWBase::Environment::get().getWorld()->getStore().get<ESM::Dialogue>();

        Filter filter (mActor, mChoice, mTalkedTo);

        MWWorld::Store<ESM::Dialogue>::iterator it = dialogs.begin();
        for (; it != dialogs.end(); ++it)
        {
            if(it->mType == ESM::Dialogue::Topic)
            {
                for (std::vector<ESM::DialInfo>::const_iterator iter (it->mInfo.begin());
                    iter!=it->mInfo.end(); ++iter)
                {
                    if (filter (*iter) && isMatching (mActor, *iter) && functionFilter(mActor,*iter,true))
                    {
                        mActorKnownTopics.push_back(toLower(it->mId));
                        //does the player know the topic?
                        if(mKnownTopics.find(toLower(it->mId)) != mKnownTopics.end())
                        {
                            keywordList.push_back(it->mId);
                            break;
                        }
                    }
                }
            }
        }

        // check the available services of this actor
        int services = 0;
        if (mActor.getTypeName() == typeid(ESM::NPC).name())
        {
            MWWorld::LiveCellRef<ESM::NPC>* ref = mActor.get<ESM::NPC>();
            if (ref->mBase->mHasAI)
                services = ref->mBase->mAiData.mServices;
        }
        else if (mActor.getTypeName() == typeid(ESM::Creature).name())
        {
            MWWorld::LiveCellRef<ESM::Creature>* ref = mActor.get<ESM::Creature>();
            if (ref->mBase->mHasAI)
                services = ref->mBase->mAiData.mServices;
        }

        int windowServices = 0;

        if (services & ESM::NPC::Weapon
            || services & ESM::NPC::Armor
            || services & ESM::NPC::Clothing
            || services & ESM::NPC::Books
            || services & ESM::NPC::Ingredients
            || services & ESM::NPC::Picks
            || services & ESM::NPC::Probes
            || services & ESM::NPC::Lights
            || services & ESM::NPC::Apparatus
            || services & ESM::NPC::RepairItem
            || services & ESM::NPC::Misc)
            windowServices |= MWGui::DialogueWindow::Service_Trade;

        if(mActor.getTypeName() == typeid(ESM::NPC).name() && !mActor.get<ESM::NPC>()->mBase->mTransport.empty())
            windowServices |= MWGui::DialogueWindow::Service_Travel;

        if (services & ESM::NPC::Spells)
            windowServices |= MWGui::DialogueWindow::Service_BuySpells;

        if (services & ESM::NPC::Spellmaking)
            windowServices |= MWGui::DialogueWindow::Service_CreateSpells;

        if (services & ESM::NPC::Training)
            windowServices |= MWGui::DialogueWindow::Service_Training;

        if (services & ESM::NPC::Enchanting)
            windowServices |= MWGui::DialogueWindow::Service_Enchant;

        win->setServices (windowServices);

        // sort again, because the previous sort was case-sensitive
        keywordList.sort(stringCompareNoCase);
        win->setKeywords(keywordList);

        mChoice = choice;
    }

    void DialogueManager::keywordSelected (const std::string& keyword)
    {
        if(!mIsInChoice)
        {
            if(mDialogueMap.find(keyword) != mDialogueMap.end())
            {
                ESM::Dialogue ndialogue = mDialogueMap[keyword];
                if(ndialogue.mType == ESM::Dialogue::Topic)
                {
                    Filter filter (mActor, mChoice, mTalkedTo);
                
                    for (std::vector<ESM::DialInfo>::const_iterator iter  = ndialogue.mInfo.begin();
                        iter!=ndialogue.mInfo.end(); ++iter)
                    {
                        if (filter (*iter) && isMatching (mActor, *iter) && functionFilter(mActor,*iter,true))
                        {
                            std::string text = iter->mResponse;
                            std::string script = iter->mResultScript;

                            parseText(text);

                            MWGui::DialogueWindow* win = MWBase::Environment::get().getWindowManager()->getDialogueWindow();
                            win->addTitle(keyword);
                            win->addText(iter->mResponse);

                            executeScript(script);

                            mLastTopic = keyword;
                            mLastDialogue = *iter;
                            break;
                        }
                    }
                }
            }
        }

        updateTopics();
    }

    void DialogueManager::goodbyeSelected()
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_Dialogue);
    }

    void DialogueManager::questionAnswered (const std::string& answer)
    {
        if(mChoiceMap.find(answer) != mChoiceMap.end())
        {
            mChoice = mChoiceMap[answer];

            std::vector<ESM::DialInfo>::const_iterator iter;
            if(mDialogueMap.find(mLastTopic) != mDialogueMap.end())
            {
                ESM::Dialogue ndialogue = mDialogueMap[mLastTopic];
                if(ndialogue.mType == ESM::Dialogue::Topic)
                {
                    Filter filter (mActor, mChoice, mTalkedTo);
                
                    for (std::vector<ESM::DialInfo>::const_iterator iter = ndialogue.mInfo.begin();
                        iter!=ndialogue.mInfo.end(); ++iter)
                    {
                        if (filter (*iter) && isMatching (mActor, *iter) && functionFilter(mActor,*iter,true))
                        {
                            mChoiceMap.clear();
                            mChoice = -1;
                            mIsInChoice = false;
                            MWGui::DialogueWindow* win = MWBase::Environment::get().getWindowManager()->getDialogueWindow();
                            std::string text = iter->mResponse;
                            parseText(text);
                            win->addText(text);
                            executeScript(iter->mResultScript);
                            mLastTopic = mLastTopic;
                            mLastDialogue = *iter;
                            break;
                        }
                    }
                }
            }
            updateTopics();
        }
    }

    void DialogueManager::printError (std::string error)
    {
        MWGui::DialogueWindow* win = MWBase::Environment::get().getWindowManager()->getDialogueWindow();
        win->addText(error);
    }

    void DialogueManager::askQuestion (const std::string& question, int choice)
    {
        MWGui::DialogueWindow* win = MWBase::Environment::get().getWindowManager()->getDialogueWindow();
        win->askQuestion(question);
        mChoiceMap[toLower(question)] = choice;
        mIsInChoice = true;
    }

    std::string DialogueManager::getFaction() const
    {
        if (mActor.getTypeName() != typeid(ESM::NPC).name())
            return "";

        std::string factionID("");
        MWMechanics::NpcStats stats = MWWorld::Class::get(mActor).getNpcStats(mActor);
        if(stats.getFactionRanks().empty())
        {
            std::cout << "No faction for this actor!";
        }
        else
        {
            factionID = stats.getFactionRanks().begin()->first;
        }
        return factionID;
    }

    void DialogueManager::goodbye()
    {
        MWGui::DialogueWindow* win = MWBase::Environment::get().getWindowManager()->getDialogueWindow();

        win->goodbye();
    }
}
