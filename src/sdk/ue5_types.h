#pragma once
#include <cstdint>
#include <cmath>

// UE5 uses double-precision vectors (changed from float in UE4)
struct FVector {
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;

    FVector operator-(const FVector& other) const {
        return {X - other.X, Y - other.Y, Z - other.Z};
    }

    FVector operator+(const FVector& other) const {
        return {X + other.X, Y + other.Y, Z + other.Z};
    }

    FVector operator*(double scalar) const {
        return {X * scalar, Y * scalar, Z * scalar};
    }

    double Length() const {
        return std::sqrt(X * X + Y * Y + Z * Z);
    }

    double DistanceTo(const FVector& other) const {
        return (*this - other).Length();
    }

    // Distance in meters (UE uses centimeters internally)
    double DistanceToMeters(const FVector& other) const {
        return DistanceTo(other) / 100.0;
    }

    double Dot(const FVector& other) const {
        return X * other.X + Y * other.Y + Z * other.Z;
    }

    FVector Cross(const FVector& other) const {
        return {
            Y * other.Z - Z * other.Y,
            Z * other.X - X * other.Z,
            X * other.Y - Y * other.X
        };
    }
};

struct FVector2D {
    float X = 0.0f;
    float Y = 0.0f;
};

struct FRotator {
    double Pitch = 0.0;
    double Yaw = 0.0;
    double Roll = 0.0;
};

struct FTransform {
    // Quaternion rotation
    double RotationX = 0.0, RotationY = 0.0, RotationZ = 0.0, RotationW = 1.0;
    // Translation
    double TranslationX = 0.0, TranslationY = 0.0, TranslationZ = 0.0;
    double TranslationPad = 0.0; // Padding
    // Scale
    double ScaleX = 1.0, ScaleY = 1.0, ScaleZ = 1.0;
    double ScalePad = 0.0; // Padding

    FVector GetTranslation() const {
        return {TranslationX, TranslationY, TranslationZ};
    }

    FVector GetScale() const {
        return {ScaleX, ScaleY, ScaleZ};
    }

    // Transforms a local coordinate vector by this transform (Scale -> Rotate via Quat -> Translate)
    FVector TransformPosition(const FVector& point) const {
        // 1. Scale
        FVector scaled(point.X * ScaleX, point.Y * ScaleY, point.Z * ScaleZ);

        // 2. Rotate via Quaternion
        FVector qvec(RotationX, RotationY, RotationZ);
        FVector uv = qvec.Cross(scaled);
        FVector uuv = qvec.Cross(uv);
        FVector rotated = scaled + (uv * (2.0 * RotationW)) + (uuv * 2.0);

        // 3. Translate
        return rotated + FVector(TranslationX, TranslationY, TranslationZ);
    }
};

namespace BoneIndices {
    constexpr int Root       = 0;
    constexpr int Pelvis     = 1;
    constexpr int Spine01    = 2;
    constexpr int Spine02    = 3;
    constexpr int Spine03    = 4; // Lower chest
    constexpr int Chest      = 5; // Upper chest / spine_05
    constexpr int Neck       = 6; // neck_01
    constexpr int Head       = 7; // head (or 67 in full skeleton)

    // Full humanoid skeleton constants
    constexpr int Humanoid_Head  = 67;
    constexpr int Humanoid_Neck  = 66;
    constexpr int Humanoid_Chest = 4;
}

template<typename T>
struct TArray {
    uintptr_t Data = 0;   // Pointer to contiguous array
    int32_t Count = 0;     // Current element count
    int32_t Max = 0;       // Allocated capacity
};

struct FName {
    int32_t ComparisonIndex = 0;  // Index into GNames pool
    int32_t Number = 0;           // Instance number (0 = no suffix)
};

struct FMinimalViewInfo {
    FVector Location;    // 0x00 (24 bytes)
    FRotator Rotation;   // 0x18 (24 bytes)
    float FOV;           // 0x30
};

// D3D/Math matrix for world-to-screen
struct FMatrix {
    float M[4][4] = {};

    FMatrix operator*(const FMatrix& other) const {
        FMatrix result;
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                result.M[i][j] = 0;
                for (int k = 0; k < 4; k++)
                    result.M[i][j] += M[i][k] * other.M[k][j];
            }
        return result;
    }
};

// UObject base layout — offsets from the validated Linux SDK dump.
// Source: dumps/sdk-validated-4888/external-linux-DungeonCrawler.exe/CppSDK/
// NOTE: These may shift between game patches. If actors stop loading,
// re-deploy proxy + Dumper-7 to regenerate SDK dump and update offsets.
namespace UEOffsets {
    // ========================
    //  UObject (base class, 0x28 bytes)
    // ========================
    constexpr uintptr_t UObject_VTable      = 0x00;
    constexpr uintptr_t UObject_Flags       = 0x08;
    constexpr uintptr_t UObject_Index       = 0x0C;
    constexpr uintptr_t UObject_Class       = 0x10;   // UClass*
    constexpr uintptr_t UObject_Name        = 0x18;   // FName
    constexpr uintptr_t UObject_Outer       = 0x20;

    // ========================
    //  AActor (0x02A8 total, inherits UObject)
    //  Source: Engine_classes.hpp
    // ========================
    constexpr uintptr_t AActor_bHidden           = 0x0058;  // bit 7
    constexpr uintptr_t AActor_bBeingDestroyed   = 0x005D;  // bit 0
    constexpr uintptr_t AActor_Owner             = 0x0158;  // AActor*
    constexpr uintptr_t AActor_RootComponent     = 0x01B8;  // USceneComponent*

    // ========================
    //  USceneComponent
    //  Source: Engine_classes.hpp
    // ========================
    constexpr uintptr_t SceneComp_RelativeLocation = 0x0148;  // FVector (24 bytes)
    constexpr uintptr_t SceneComp_RelativeRotation = 0x0160;  // FRotator
    constexpr uintptr_t SceneComp_ComponentVelocity = 0x0190; // FVector
    constexpr uintptr_t SceneComp_ComponentToWorld = 0x01F0;  // FTransform; live translation at +0x210

    // ========================
    //  UWorld (0x0AE8 total)
    //  Source: Engine_classes.hpp, validated Linux SDK
    // ========================
    constexpr uintptr_t UWorld_PersistentLevel    = 0x0030;  // ULevel*
    constexpr uintptr_t UWorld_GameState          = 0x01B0;  // AGameStateBase*
    constexpr uintptr_t UWorld_Levels             = 0x01C8;  // TArray<ULevel*>
    constexpr uintptr_t UWorld_OwningGameInstance = 0x0228;  // UGameInstance*

    // ========================
    //  ULevel (inherits UObject)
    //  Source: Engine_classes.hpp line 8836
    //  Actors array — auto-detected at 0xA0 (was 0x98 in older builds)
    //  Verified: 25501 actors across 148 levels in Goblin Caves dungeon
    // ========================
    constexpr uintptr_t ULevel_Actors = 0xA0;   // TArray<AActor*> (hidden in padding)

    // ========================
    //  UGameInstance -> ULocalPlayer -> APlayerController chain
    //  Source: Engine_classes.hpp
    // ========================
    constexpr uintptr_t GameInstance_LocalPlayers           = 0x0038;  // TArray<ULocalPlayer*>
    constexpr uintptr_t LocalPlayer_PlayerController       = 0x0030;  // APlayerController*
    constexpr uintptr_t PlayerController_AcknowledgedPawn  = 0x0350;  // APawn*
    constexpr uintptr_t PlayerController_PlayerCameraManager = 0x0360; // APlayerCameraManager*
    constexpr uintptr_t PlayerController_ControlRotation   = 0x0320; // FRotator (ControlRotation)

    // ========================
    //  APlayerCameraManager
    //  Source: Engine_classes.hpp line 26586
    // ========================
    constexpr uintptr_t CameraManager_CameraCachePrivate = 0x15D0;  // FCameraCacheEntry
    constexpr uintptr_t CameraCache_POV = 0x10;  // FMinimalViewInfo (0x910 bytes)

    // ========================
    //  Projectile Movement Component
    //  Source: Engine_classes.hpp
    // ========================
    constexpr uintptr_t ProjectileActor_ProjectileMovement = 0x0330; // UProjectileMovementComponent*
    constexpr uintptr_t MovementComponent_Velocity = 0x00D8;       // FVector (Velocity in UMovementComponent)

    // ========================
    //  Dark and Darker: ADCCharacterBase (0x0B30 total)
    //  Source: DungeonCrawler_classes.hpp
    //  Inherits: ACharacter (0x0670) → ADCCharacterBase
    // ========================
    constexpr uintptr_t DCChar_AbilitySystemComponent = 0x0730;  // UAbilitySystemComponent*
    constexpr uintptr_t DCChar_GenericTeamId          = 0x0748;  // FGenericTeamId
    constexpr uintptr_t DCChar_AccountId              = 0x0800;  // FString
    constexpr uintptr_t DCChar_AccountDataReplication = 0x0820;  // FAccountDataReplication (0xE8)
    constexpr uintptr_t DCChar_bIsDead                = 0x090A;  // bool
    constexpr uintptr_t DCChar_AccountDataReplicationCached = 0x0930;  // FAccountDataReplication (Net)
    constexpr uintptr_t DCChar_InventoryComponentV2   = 0x0A90;  // UDCInventoryComponent*

    // ========================
    //  ADCGameStateBase party replication (SDK 4888)
    // ========================
    constexpr uintptr_t GameState_PartyDataArray = 0x0698;  // TArray<FPartyData>

    // ========================
    //  FAccountDataReplication (0x00E8 total)
    //  Source: DungeonCrawler_structs.hpp line 5032
    //  Read from: actorAddr + DCChar_AccountDataReplication + offset
    // ========================
    constexpr uintptr_t AcctData_AccountId   = 0x0000;  // FString
    constexpr uintptr_t AcctData_Nickname          = 0x0010;  // FNickname (0x98)
    constexpr uintptr_t AcctData_PlayerCharacterId = 0x00A8;  // FString (class asset ID)
    constexpr uintptr_t AcctData_PartyId           = 0x00B8;  // FString (empty when solo)
    constexpr uintptr_t AcctData_CharacterId       = 0x00C8;  // FString (character ID)
    constexpr uintptr_t AcctData_Gender            = 0x00D8;  // int32
    constexpr uintptr_t AcctData_Level             = 0x00DC;  // int32
    constexpr uintptr_t AcctData_bAlive            = 0x00E3;  // bool
    constexpr uintptr_t AcctData_bDown             = 0x00E5;  // bool

    // ========================
    //  FNickname (0x98 total)
    //  Source: DungeonCrawler_structs.hpp line 4231
    // ========================
    constexpr uintptr_t Nickname_OriginalNickName = 0x0000;  // FString (the player's display name)

    // ========================
    //  ADCPlayerCharacterBase (inherits ADCCharacterBase)
    //  Source: DungeonCrawler_classes.hpp
    // ========================
    constexpr uintptr_t DCPlayer_CharacterKey         = 0x0BB8;  // FDCPlayerCharacterKey (two uint8 fields)
    constexpr uintptr_t DCPlayer_DataComponent        = 0x0C08;
    constexpr uintptr_t DCPlayer_EquipmentComponentV2 = 0x0C20;

    // ========================
    //  UAbilitySystemComponent -> UDCAttributeSet health chain
    //  Source: GameplayAbilities_classes.hpp, DungeonCrawler_classes.hpp
    // ========================
    constexpr uintptr_t ASC_SpawnedAttributes = 0x1080;  // TArray<UAttributeSet*>

    // ========================
    //  UDCAttributeSet (0x0C78 total, inherits UDCAttributeSetBase at 0x30)
    //  Source: DungeonCrawler_classes.hpp line ~21050
    //  FGameplayAttributeData is 0x10 bytes: [pad 0x8][BaseValue float][CurrentValue float]
    // ========================
    constexpr uintptr_t Attr_Strength        = 0x0030;  // FGameplayAttributeData
    constexpr uintptr_t Attr_Vigor           = 0x0060;
    constexpr uintptr_t Attr_Agility         = 0x0090;
    constexpr uintptr_t Attr_RecoverableHealth = 0x0810;
    constexpr uintptr_t Attr_Health          = 0x0820;  // FGameplayAttributeData
    constexpr uintptr_t Attr_OverhealedHealth = 0x0830;
    constexpr uintptr_t Attr_MaxHealth       = 0x0840;  // FGameplayAttributeData
    // FGameplayAttributeData layout: +0x08 = BaseValue (float), +0x0C = CurrentValue (float)
    constexpr uintptr_t AttrData_BaseValue    = 0x08;
    constexpr uintptr_t AttrData_CurrentValue = 0x0C;

    // ========================
    //  UDCMonsterDmgIndicatorComponent (simple health)
    //  Source: DungeonCrawler_classes.hpp
    // ========================
    constexpr uintptr_t MonsterDmg_Health    = 0x00D4;  // float
    constexpr uintptr_t MonsterDmg_MaxHealth = 0x00D8;  // float

    // ========================
    //  AItemHolderActorBase (items on ground)
    //  Source: DungeonCrawler_classes.hpp line 18897
    //  Inherits: ADCInteractableActorBase (0x0318)
    // ========================
    constexpr uintptr_t ItemHolder_MeshComponent = 0x0340;  // UMeshComponent*
    constexpr uintptr_t ItemHolder_ItemId        = 0x0348;  // FPrimaryAssetId (0x10)
    constexpr uintptr_t ItemHolder_DataAsset     = 0x0358;  // UDCItemDataAsset*
    constexpr uintptr_t ItemHolder_ItemInfo      = 0x0360;  // FDCItemInfo (0x278)

    // ========================
    //  UDCItemDataAsset (0x270 total)
    //  Source: DungeonCrawler_classes.hpp line 34117
    // ========================
    constexpr uintptr_t ItemData_IdTag      = 0x0078;  // FGameplayTag
    constexpr uintptr_t ItemData_Name       = 0x00A8;  // FText
    constexpr uintptr_t ItemData_ItemType   = 0x00C8;  // EItemType (uint8)
    constexpr uintptr_t ItemData_RarityType = 0x0110;  // FGameplayTag
    constexpr uintptr_t ItemData_GearScore  = 0x0260;  // int32

    // ========================
    //  FDCItemInfo (0x278 total, carried/ground item state)
    //  Source: DungeonCrawler_structs.hpp line 4653
    // ========================
    constexpr uintptr_t ItemInfo_ID        = 0x0008;  // FDCItemId (0x08)
    constexpr uintptr_t ItemInfo_DataAsset = 0x0010;  // UDCItemDataAsset*
    constexpr uintptr_t ItemInfo_Stack     = 0x0028;  // int32

    // ========================
    //  UDCInventoryComponent (character inventory system)
    //  Source: DungeonCrawler_classes.hpp
    //  Inherits: UDCInventoryContainerComponent (0x0170 base)
    // ========================
    constexpr uintptr_t InvContainer_InventoryList = 0x0190;  // TArray<UDCInventoryBase*>

    // ========================
    //  UDCInventoryBase (inventory slot container)
    //  Source: DungeonCrawler_classes.hpp
    // ========================
    constexpr uintptr_t InvBase_InventoryId     = 0x0088;  // EDCInventoryId (uint8)
    constexpr uintptr_t InvBase_InventoryData   = 0x0148;  // FDCInventoryData

    // ========================
    //  FDCInventoryData -> fast array of items
    //  Source: DungeonCrawler_structs.hpp
    //  Contains FFastArraySerializer header then TArray<FDCInventoryDataElement>
    // ========================
    constexpr uintptr_t InvData_Items           = 0x0110;  // TArray<FDCInventoryDataElement> within FDCInventoryData

    // ========================
    //  FDCInventoryDataElement (one inventory slot)
    //  Source: DungeonCrawler_structs.hpp
    // ========================
    constexpr uintptr_t InvDataElem_Index       = 0x000C;  // int32 (slot index)
    constexpr uintptr_t InvDataElem_ItemInfo    = 0x0010;  // FDCItemInfo (0x228)
    constexpr uintptr_t InvDataElem_Size        = 0x0288;  // Total size per element

    // ========================
    //  Historical monster loot TArray fallback (runtime-discovered, Feb 2026)
    //  Monster bodies use invId=2. The items TArray lives at invBase+0xD8
    //  (NOT at the standard InvBase_InventoryData + InvData_Items = 0x0238).
    //  Element layout: size=0x240, DataAsset at elem+0x08+ItemInfo_DataAsset(0x10)=elem+0x18.
    //  Confirmed: DeathSkull(Arrow), SkeletonArcher(IntactSkull), RuinsGolem(Armet), dead Players.
    // ========================
    constexpr uintptr_t InvBase_MonsterItems    = 0x00D8;  // TArray<FDCInventoryDataElement> (monster invId=2)
    constexpr uintptr_t InvDataElem_ItemInfoAlt = 0x0008;  // FDCItemInfo offset in monster loot elements

    // EDCInventoryId values
    constexpr uint8_t INVENTORY_ID_EQUIPMENT    = 3;

    // ========================
    //  ADCMonsterBase (inherits ADCCharacterBase at 0x0B30)
    //  Source: DungeonCrawler_classes.hpp line 4942
    // ========================
    constexpr uintptr_t DCMonster_MonsterId              = 0x0C30;  // FPrimaryAssetId
    constexpr uintptr_t DCMonster_DesignDataAssetMonster = 0x0DB8;  // UDesignDataAssetMonster*

    // ========================
    //  UDesignDataAssetMonster -> FDesignDataMonster
    //  Source: DungeonCrawler_classes.hpp line 16312, DungeonCrawler_structs.hpp line 11178
    // ========================
    constexpr uintptr_t MonsterDataAsset_Item   = 0x0068;  // FDesignDataMonster struct
    constexpr uintptr_t DesignMonster_IdTag     = 0x0000;  // FGameplayTag
    constexpr uintptr_t DesignMonster_ClassType = 0x0008;  // FGameplayTag
    constexpr uintptr_t DesignMonster_GradeType = 0x0010;  // FGameplayTag (Common/Elite/Nightmare)
    constexpr uintptr_t DesignMonster_Name      = 0x0028;  // FText

    // ========================
    //  ACharacter -> USkeletalMeshComponent (bone-based ESP)
    //  Source: Engine_classes.hpp line 30008 (ACharacter::Mesh at 0x0328)
    //  Source: Engine_classes.hpp line 5789 (USkeletalMeshComponent)
    // ========================
    constexpr uintptr_t ACharacter_Mesh = 0x0328;  // USkeletalMeshComponent*

    // USkeletalMeshComponent bone transform arrays
    constexpr uintptr_t SkelMesh_CachedComponentSpaceTransforms = 0x0A58; // TArray<FTransform>

    // FTransform is 0x60 bytes (96 bytes) per bone
    constexpr size_t FTransform_Size = 0x60;
    // Translation offset within FTransform (after quaternion rotation: 4 doubles = 32 bytes)
    constexpr uintptr_t FTransform_Translation = 0x20;

    // ========================
    //  UClass hierarchy traversal (for IsA checks)
    // ========================
    constexpr uintptr_t UStruct_SuperStruct = 0x40;
}

// ========================
//  Dark and Darker character class enum
// ========================
enum class EDCCharacterClass : uint8_t {
    None      = 0,
    Wizard    = 1,
    Rogue     = 2,
    Ranger    = 3,
    Fighter   = 4,
    Cleric    = 5,
    Barbarian = 6,
    Bard      = 7,
    Warlock   = 8,
    Druid     = 9,
    Sorcerer  = 10,
    GrandMaster = 11
};

inline const char* CharacterClassToString(EDCCharacterClass cls) {
    switch (cls) {
        case EDCCharacterClass::Wizard:    return "Wizard";
        case EDCCharacterClass::Rogue:     return "Rogue";
        case EDCCharacterClass::Ranger:    return "Ranger";
        case EDCCharacterClass::Fighter:   return "Fighter";
        case EDCCharacterClass::Cleric:    return "Cleric";
        case EDCCharacterClass::Barbarian: return "Barbarian";
        case EDCCharacterClass::Bard:      return "Bard";
        case EDCCharacterClass::Warlock:   return "Warlock";
        case EDCCharacterClass::Druid:     return "Druid";
        case EDCCharacterClass::Sorcerer:  return "Sorcerer";
        case EDCCharacterClass::GrandMaster: return "GrandMaster";
        default:                           return "Unknown";
    }
}

// ========================
//  EDCItemRarity (from SDK: DungeonCrawler_structs.hpp)
// ========================
enum class EDCItemRarity : uint8_t {
    None     = 0,
    Poor     = 1,
    Common   = 2,
    Uncommon = 3,
    Rare     = 4,
    Epic     = 5,
    Legend   = 6,
    Unique   = 7,
    Artifact = 8
};

// ========================
//  EDCItemType (from SDK: DungeonCrawler_structs.hpp)
// ========================
enum class EDCItemType : uint8_t {
    None      = 0,
    Weapon    = 1,
    Armor     = 2,
    Utility   = 3,
    Accessory = 4,
    Misc      = 5,
    Misc_Gem  = 6
};

// ========================
//  FString layout for reading UE FString from memory
//  Data pointer + int32 Count + int32 Max
// ========================
struct FString {
    uintptr_t Data = 0;   // UTF-16 code-unit pointer in the Proton game
    int32_t Count = 0;     // String length (including null terminator)
    int32_t Max = 0;       // Allocated capacity
};
