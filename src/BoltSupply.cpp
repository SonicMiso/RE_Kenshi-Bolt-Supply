#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include <Debug.h>
#include <core/Functions.h>

#include <kenshi/Building/Building.h>
#include <kenshi/Character.h>
#include <kenshi/Enums.h>
#include <kenshi/Faction.h>
#include <kenshi/Gear.h>
#include <kenshi/GunClass.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/Inventory.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RootObject.h>

namespace BoltSupply
{
    static const float SEARCH_RADIUS = 100.0f;
    static const float ARRIVAL_RADIUS = 3.0f;
    static const int LOW_AMMO_THRESHOLD = 5;
    static const int DONOR_RESERVE = 5;
    static const int REFILL_TARGET = 10;

    enum class JobState
    {
        None,
        Travelling
    };

    struct Job
    {
        JobState state;
        RootObject* source;
        GameData* ammoType;

        Job()
            : state(JobState::None)
            , source(nullptr)
            , ammoType(nullptr)
        {
        }
    };

    static std::unordered_map<Character*, Job> jobs;
    static std::unordered_set<RootObject*> reservedSources;
    static float scanTimer = 0.0f;

    static int ammoCount(Inventory* inventory, GameData* ammoType)
    {
        if (!inventory || !ammoType)
            return 0;

        lektor<Item*> ammo;
        inventory->getAllItemsWithFunction(ammo, ITEM_AMMO);

        int total = 0;
        for (int i = 0; i < ammo.size(); ++i)
        {
            Item* item = ammo[i];
            if (item && item->getGameData() == ammoType)
                total += std::max(0, item->quantity);
        }

        return total;
    }

    static GameData* requiredAmmoType(Crossbow* crossbow)
    {
        if (!crossbow || !crossbow->gunClass)
            return nullptr;

        // GunClass::ammoType is the game's authoritative ammo definition.
        // Matching the GameData pointer avoids mixing Regular, Toothpick and
        // Long/Heavy Bolts even though all of them are ITEM_AMMO.
        return crossbow->gunClass->ammoType;
    }

    static bool isPlayerCharacter(Character* character)
    {
        if (!character || !ou || !ou->player)
            return false;

        const lektor<Character*>& characters = ou->player->getAllPlayerCharacters();
        for (int i = 0; i < characters.size(); ++i)
            if (characters[i] == character)
                return true;

        return false;
    }

    static bool isFriendlyContainer(RootObject* object)
    {
        if (!object || !ou || !ou->player)
            return false;

        Faction* playerFaction = ou->player->getFaction();
        Faction* objectFaction = object->getFaction();

        return playerFaction != nullptr &&
               objectFaction != nullptr &&
               objectFaction == playerFaction;
    }

    static bool hasEnoughToDonate(Inventory* inventory, GameData* ammoType)
    {
        return ammoCount(inventory, ammoType) > DONOR_RESERVE;
    }

    static bool isReserved(RootObject* object)
    {
        return object && reservedSources.find(object) != reservedSources.end();
    }

    static Item* chooseAmmoStack(Inventory* inventory, GameData* ammoType, int maxTransfer)
    {
        if (!inventory || !ammoType || maxTransfer <= 0)
            return nullptr;

        lektor<Item*> ammo;
        inventory->getAllItemsWithFunction(ammo, ITEM_AMMO);

        Item* best = nullptr;
        for (int i = 0; i < ammo.size(); ++i)
        {
            Item* item = ammo[i];
            if (!item || item->quantity <= 0 || item->getGameData() != ammoType)
                continue;

            if (!best || item->quantity > best->quantity)
                best = item;
        }

        return best;
    }

    static bool transferAmmo(Character* receiver, RootObject* source, GameData* ammoType)
    {
        if (!receiver || !source || !ammoType)
            return false;

        Inventory* receiverInventory = receiver->getInventory();
        Inventory* sourceInventory = source->getInventory();

        if (!receiverInventory || !sourceInventory)
            return false;

        int receiverAmmo = ammoCount(receiverInventory, ammoType);
        if (receiverAmmo >= LOW_AMMO_THRESHOLD)
            return true;

        int sourceAmmo = ammoCount(sourceInventory, ammoType);
        int transferable = sourceAmmo - DONOR_RESERVE;
        if (transferable <= 0)
            return false;

        int wanted = REFILL_TARGET - receiverAmmo;
        if (wanted <= 0)
            return true;

        int amount = std::min(wanted, transferable);
        Item* stack = chooseAmmoStack(sourceInventory, ammoType, amount);
        if (!stack)
            return false;

        amount = std::min(amount, stack->quantity);
        if (amount <= 0)
            return false;

        // The movement/order has already brought the receiver to the source.
        // Only now do we perform the actual inventory operation.
        Item* moved = sourceInventory->removeItemDontDestroy_returnsItem(
            stack,
            amount,
            true);

        if (!moved)
            return false;

        if (!receiverInventory->tryAddItem(moved, amount))
        {
            sourceInventory->tryAddItem(moved, amount);
            return false;
        }

        DebugLog("Bolt Supply: ammo transfer completed");

        return true;
    }

    static RootObject* findContainerSource(Character* receiver, GameData* ammoType)
    {
        if (!ou || !ou->player || !receiver || !ammoType)
            return nullptr;

        lektor<RootObject*> objects;
        ou->getObjectsWithinSphere(
            objects,
            receiver->getPosition(),
            SEARCH_RADIUS,
            CONTAINER,
            128,
            receiver);

        RootObject* best = nullptr;
        float bestDistance = 0.0f;

        for (int i = 0; i < objects.size(); ++i)
        {
            RootObject* object = objects[i];
            if (!object || isReserved(object) || !isFriendlyContainer(object))
                continue;

            Inventory* inventory = object->getInventory();
            if (!inventory || !hasEnoughToDonate(inventory, ammoType))
                continue;

            float distance = receiver->getPosition().squaredDistance(object->getPosition());
            if (!best || distance < bestDistance)
            {
                best = object;
                bestDistance = distance;
            }
        }

        return best;
    }

    static RootObject* findTeammateSource(Character* receiver, GameData* ammoType)
    {
        if (!ou || !ou->player || !receiver || !ammoType)
            return nullptr;

        const lektor<Character*>& characters = ou->player->getAllPlayerCharacters();

        Character* best = nullptr;
        float bestDistance = 0.0f;

        for (int i = 0; i < characters.size(); ++i)
        {
            Character* candidate = characters[i];
            if (!candidate || candidate == receiver)
                continue;

            if (!isPlayerCharacter(candidate))
                continue;

            if (isReserved(candidate))
                continue;

            Inventory* inventory = candidate->getInventory();
            if (!inventory || !hasEnoughToDonate(inventory, ammoType))
                continue;

            float distance = receiver->getPosition().squaredDistance(candidate->getPosition());
            if (distance > SEARCH_RADIUS * SEARCH_RADIUS)
                continue;

            if (!best || distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }

        return best;
    }

    static RootObject* findSource(Character* receiver, GameData* ammoType)
    {
        RootObject* container = findContainerSource(receiver, ammoType);
        RootObject* teammate = findTeammateSource(receiver, ammoType);

        if (!container)
            return teammate;

        if (!teammate)
            return container;

        float containerDistance =
            receiver->getPosition().squaredDistance(container->getPosition());
        float teammateDistance =
            receiver->getPosition().squaredDistance(teammate->getPosition());

        return containerDistance <= teammateDistance ? container : teammate;
    }

    static void clearJob(Character* character)
    {
        auto it = jobs.find(character);
        if (it == jobs.end())
            return;

        if (it->second.source)
            reservedSources.erase(it->second.source);

        jobs.erase(it);
    }

    static void startJob(Character* character, RootObject* source, GameData* ammoType)
    {
        if (!character || !source || !ammoType)
            return;

        if (jobs.find(character) != jobs.end())
            return;

        if (isReserved(source))
            return;

        Job job;
        job.state = JobState::Travelling;
        job.source = source;
        job.ammoType = ammoType;

        jobs[character] = job;
        reservedSources.insert(source);

        // Queue a native movement order rather than teleporting or directly
        // moving inventory. The transfer is performed only after arrival.
        character->addOrder(
            nullptr,
            MOVE_CUS_ORDERED,
            source,
            true,
            true,
            source->getPosition());

        DebugLog("Bolt Supply: character travelling to supply source");
    }

    static void updateJob(Character* character)
    {
        auto it = jobs.find(character);
        if (it == jobs.end())
            return;

        Job& job = it->second;
        RootObject* source = job.source;

        if (!character || !source || source->isDestroyed())
        {
            clearJob(character);
            return;
        }

        if (ammoCount(character->getInventory(), job.ammoType) >= LOW_AMMO_THRESHOLD)
        {
            clearJob(character);
            return;
        }

        float distance = character->getPosition().squaredDistance(source->getPosition());
        if (distance > ARRIVAL_RADIUS * ARRIVAL_RADIUS)
            return;

        transferAmmo(character, source, job.ammoType);
        clearJob(character);
    }

    static void update(float delta)
    {
        if (!ou || !ou->player || !ou->initialized)
            return;

        scanTimer += delta;

        // Jobs are checked every frame so arrival is responsive.
        for (auto it = jobs.begin(); it != jobs.end(); )
        {
            Character* character = it->first;
            ++it;
            updateJob(character);
        }

        if (scanTimer < 0.5f)
            return;

        scanTimer = 0.0f;

        const lektor<Character*>& characters =
            ou->player->getAllPlayerCharacters();

        for (int i = 0; i < characters.size(); ++i)
        {
            Character* character = characters[i];
            if (!character || !character->getInventory())
                continue;

            if (jobs.find(character) != jobs.end())
                continue;

            Weapon* current = character->getCurrentWeapon();
            Crossbow* crossbow = current ? current->isCrossbow() : nullptr;
            if (!crossbow)
                continue;

            GameData* ammoType = requiredAmmoType(crossbow);
            if (!ammoType)
                continue;

            int ammo = ammoCount(character->getInventory(), ammoType);
            if (ammo >= LOW_AMMO_THRESHOLD)
                continue;

            RootObject* source = findSource(character, ammoType);
            if (!source)
                continue;

            startJob(character, source, ammoType);
        }
    }
}

static void (*mainLoopOriginal)(GameWorld*, float);

static void mainLoopHook(GameWorld* world, float time)
{
    mainLoopOriginal(world, time);
    BoltSupply::update(time);
}

__declspec(dllexport) void startPlugin()
{
    DebugLog("Bolt Supply: startPlugin entered");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            &mainLoopHook,
            &mainLoopOriginal))
    {
        ErrorLog("Bolt Supply: could not install main loop hook");
        return;
    }

    DebugLog("Bolt Supply: enabled");
}
