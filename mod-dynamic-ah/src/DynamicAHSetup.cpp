#include "DynamicAHSetup.h"

#include "AccountMgr.h"
#include "CharacterDatabase.h"
#include "DatabaseEnv.h"
#include "DynamicAHTypes.h" // CFG_* keys
#include "Log.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h" // sObjectMgr
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSocket.h"

#include <memory>
#include <string>

namespace
{

    // --- helpers (internal only) ---

    static char const *ResultToCStr(AccountOpResult r)
    {
        switch (r)
        {
        case AOR_OK:
            return "OK";
        case AOR_NAME_TOO_LONG:
            return "NAME_TOO_LONG";
        case AOR_PASS_TOO_LONG:
            return "PASS_TOO_LONG";
        case AOR_EMAIL_TOO_LONG:
            return "EMAIL_TOO_LONG";
        case AOR_NAME_ALREADY_EXIST:
            return "NAME_ALREADY_EXIST";
        case AOR_NAME_NOT_EXIST:
            return "NAME_NOT_EXIST";
        case AOR_DB_INTERNAL_ERROR:
            return "DB_INTERNAL_ERROR";
        default:
            return "UNKNOWN";
        }
    }

    static uint32 EnsureAccount(std::string const &name, std::string const &pass, std::string const &email)
    {
        if (uint32 id = AccountMgr::GetId(name))
            return id;

        AccountOpResult res = sAccountMgr->CreateAccount(name, pass, email);
        if (res != AOR_OK && res != AOR_NAME_ALREADY_EXIST)
        {
            LOG_ERROR("mod.dynamicah", "CreateAccount('{}') failed: {}", name, ResultToCStr(res));
            return 0;
        }

        uint32 id = AccountMgr::GetId(name);
        if (!id)
            LOG_ERROR("mod.dynamicah", "CreateAccount('{}') yielded 0 id", name);

        return id;
    }

    static uint32 FindCharGuidByName(std::string const &name)
    {
        if (name.empty())
            return 0;

        std::string esc = name;
        CharacterDatabase.EscapeString(esc);
        std::string sql = "SELECT guid FROM characters WHERE name = '" + esc + "'";
        if (QueryResult r = CharacterDatabase.Query(sql.c_str()))
            return r->Fetch()[0].Get<uint32>();

        return 0;
    }

    struct CreateInfoPublic : public CharacterCreateInfo
    {
        void SetBasics(std::string const &n, uint8 race, uint8 cls, uint8 gender)
        {
            Name = n;
            Race = race;
            Class = cls;
            Gender = gender;
            Skin = Face = HairStyle = HairColor = FacialHair = 0;
            OutfitId = 0;
            CharCount = 0;
        }
    };

    static std::unique_ptr<WorldSession> MakeEphemeralSession(uint32 accountId, std::string const &accountName)
    {
        std::shared_ptr<WorldSocket> sock; // null socket is fine
        AccountTypes sec = AccountTypes(SEC_PLAYER);
        uint8 expansion = uint8(sWorld->getIntConfig(CONFIG_EXPANSION));
        time_t mute_time = 0;
        LocaleConstant locale = LocaleConstant(LOCALE_enUS);
        uint32 recruiter = 0;
        bool isARecruiter = false;
        bool skipQueue = true;
        uint32 totalTime = 0;

        uint32 accountFlags = 0;

        bool isBot = true;

        std::string nameCopy = accountName;

        return std::make_unique<WorldSession>(
            accountId,
            std::move(nameCopy),
            accountFlags,
            sock,
            sec,
            expansion,
            mute_time,
            locale,
            recruiter,
            isARecruiter,
            skipQueue,
            totalTime,
            isBot);
    }

    static uint32 CreateCharacter(uint32 accountId, std::string const &name, uint8 race, uint8 cls, uint8 gender)
    {
        std::string accName;
        (void)AccountMgr::GetName(accountId, accName);

        auto session = MakeEphemeralSession(accountId, accName);

        CreateInfoPublic info;
        info.SetBasics(name, race, cls, gender);

        std::shared_ptr<Player> newChar(new Player(session.get()), [](Player *p)
                                        {
        if (p->HasAtLoginFlag(AT_LOGIN_FIRST))
            p->CleanupsBeforeDelete();
        delete p; });

        newChar->GetMotionMaster()->Initialize();

        ObjectGuid::LowType lowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
        if (!newChar->Create(lowGuid, &info))
        {
            LOG_ERROR("mod.dynamicah", "CreateCharacter failed for '{}'", name);
            return 0;
        }

        newChar->SetAtLoginFlag(AT_LOGIN_FIRST);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        newChar->SaveToDB(trans, true, false);
        CharacterDatabase.CommitTransaction(trans);

        LOG_INFO("mod.dynamicah", "Created character '{}' guid={}", name, lowGuid);
        return (uint32)lowGuid;
    }

} // anonymous namespace

namespace ModDynamicAH
{
    namespace DynamicAHSetup
    {

        void RunSetup(ModuleState &s, ChatHandler *handler)
        {
            // Account config
            std::string accName = sConfigMgr->GetOption<std::string>(CFG_SETUP_ACC_NAME, "dynamicah");
            std::string accPass = sConfigMgr->GetOption<std::string>(CFG_SETUP_ACC_PASS, "change_me");
            std::string accEmail = sConfigMgr->GetOption<std::string>("ModDynamicAH.Setup.AccountEmail", "dynamicah@example.invalid");

            // Character names (A/H/N)
            std::string alliName = sConfigMgr->GetOption<std::string>(CFG_SETUP_ALLI_NAME, "AHSellerA");
            std::string hordName = sConfigMgr->GetOption<std::string>(CFG_SETUP_HORD_NAME, "AHSellerH");
            std::string neutName = sConfigMgr->GetOption<std::string>(CFG_SETUP_NEUT_NAME, "AHSellerN");

            // Races/classes/genders from config (default to Human/Orc/Undead with sensible classes)
            uint8 ar = uint8(sConfigMgr->GetOption<uint32>(CFG_SETUP_ALLI_RACE, 1));
            uint8 ac = uint8(sConfigMgr->GetOption<uint32>(CFG_SETUP_ALLI_CLASS, 1));
            uint8 ag = uint8(sConfigMgr->GetOption<uint32>(CFG_SETUP_ALLI_GENDER, 0));

            uint8 hr = uint8(sConfigMgr->GetOption<uint32>(CFG_SETUP_HORD_RACE, 2));
            uint8 hc = uint8(sConfigMgr->GetOption<uint32>(CFG_SETUP_HORD_CLASS, 1));
            uint8 hg = uint8(sConfigMgr->GetOption<uint32>(CFG_SETUP_HORD_GENDER, 0));

            uint8 nr = uint8(sConfigMgr->GetOption<uint32>(CFG_SETUP_NEUT_RACE, 5));
            uint8 nc = uint8(sConfigMgr->GetOption<uint32>(CFG_SETUP_NEUT_CLASS, 4));
            uint8 ng = uint8(sConfigMgr->GetOption<uint32>(CFG_SETUP_NEUT_GENDER, 0));

            // Ensure account exists
            uint32 accId = EnsureAccount(accName, accPass, accEmail);
            if (!accId)
            {
                if (handler)
                    handler->PSendSysMessage("动态AH: 创建或查找账户 {} 失败", accName.c_str());
                return;
            }

            auto ensureChar = [&](std::string const &n, uint8 r, uint8 c, uint8 g) -> uint32
            {
                if (uint32 guid = FindCharGuidByName(n))
                    return guid;
                return CreateCharacter(accId, n, r, c, g);
            };

            if (!s.ownerAlliance)
                s.ownerAlliance = ensureChar(alliName, ar, ac, ag);
            if (!s.ownerHorde)
                s.ownerHorde = ensureChar(hordName, hr, hc, hg);
            if (!s.ownerNeutral)
                s.ownerNeutral = ensureChar(neutName, nr, nc, ng);

            if (handler)
                handler->PSendSysMessage("动态AH: 设置完成。拥有者 联盟/部落/中立: {} / {} / {}",
                                         s.ownerAlliance, s.ownerHorde, s.ownerNeutral);

            LOG_INFO("mod.dynamicah", "Setup finished: A={} H={} N={}", s.ownerAlliance, s.ownerHorde, s.ownerNeutral);
        }

        void ResolveOwnersByName(ModuleState &s)
        {
            std::string alliName = sConfigMgr->GetOption<std::string>(CFG_SETUP_ALLI_NAME, "AHSellerA");
            std::string hordName = sConfigMgr->GetOption<std::string>(CFG_SETUP_HORD_NAME, "AHSellerH");
            std::string neutName = sConfigMgr->GetOption<std::string>(CFG_SETUP_NEUT_NAME, "AHSellerN");

            if (!s.ownerAlliance)
                s.ownerAlliance = FindCharGuidByName(alliName);
            if (!s.ownerHorde)
                s.ownerHorde = FindCharGuidByName(hordName);
            if (!s.ownerNeutral)
                s.ownerNeutral = FindCharGuidByName(neutName);

            if (s.ownerAlliance || s.ownerHorde || s.ownerNeutral)
                LOG_INFO("mod.dynamicah", "Resolved seller owners by name: A={} H={} N={}",
                         s.ownerAlliance, s.ownerHorde, s.ownerNeutral);
        }

    } // namespace DynamicAHSetup
} // namespace ModDynamicAH
