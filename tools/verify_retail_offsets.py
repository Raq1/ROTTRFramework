"""Verify ROTTR Trainer v0.9.143 retail signatures/layouts against the supported ROTTR.exe."""
import struct
import sys
from pathlib import Path

TESTS = {
    # Startup UI hide (faultrep.dll): ScaleformManager::Get, UiMenuStateLegal vtable slots 2/3 (OnEnterAsync/OnEnter)
    0x0014A590: bytes.fromhex("40 53 48 83 EC 20 65 48 8B 04 25 58 00 00 00"),
    0x00DB4458: (0x1408BB6A0).to_bytes(8, "little"),
    0x00DB4460: (0x14090A8A0).to_bytes(8, "little"),
    # Custom outfits: localstr_get, PlayerProfile::SetLastOutfitWorn / GetLastOutfitWorn / IsOutfitUnlocked,
    # Crafting::FindOutfitIndexByObjectID (globalCrafting +0x30/+0x38, 0xA8 per outfit), UIInfo::GetCharacterProfile
    0x000832D0: bytes.fromhex("48 85 D2 74 03 C6 02 01 83 F9 FF 75 10"),
    0x00867350: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 48 83 79 38 00 8B FA 48 8B D9 74 1A"),
    0x00861790: bytes.fromhex("48 8B 41 38 48 85 C0 74 07 8B 80 70 46 00 00 C3"),
    0x00862FC0: bytes.fromhex("4C 8B 41 38 4D 85 C0 74 1E 8B C2 83 E0 3F 0F B6 C8 8B C2 48 C1 E8 06 49 8B 84 C0 68 46 00 00"),
    0x004431B0: bytes.fromhex("4C 8B 15 79 42 91 02 33 C0 45 8B 4A 30 45 85 C9 74 28 4D 8B 52 38"),
    0x008B3890: bytes.fromhex("84 C9 78 23 4C 8B 05 85 AE 56 02 0F BE C1 41 0F B6 50 18 3B C2 7D 10 48 0F BE C1 48 69 C0 A8 00 00 00 49 03 40 10"),
    # Custom outfit new models: ObjectZoneManager name map insert (HashMap<HashString,uint>::operator[]),
    # ObjectZoneManager::ClearZoneNames (map at +0x158)
    0x002256C0: bytes.fromhex("48 89 5C 24 10 57 48 83 EC 30 48 8B DA 4C 8B C2 48 8D 54 24 20 48 8B F9 E8"),
    0x0022F1E0: bytes.fromhex("40 53 48 83 EC 20 48 8B D9 48 81 C1 58 01 00 00 E8"),
    # Material save: ApplyRelocations<SectionLoaderTraits> (batched tiger loads; lea r12,[s_pSection] at +0x23)
    0x0033E5A0: bytes.fromhex("48 89 5C 24 18 48 89 6C 24 20 56 41 54 41 57 48 83 EC 20 0F B6 42 04 48 8D 59 14 48 8B F1 4C 89 74 24 48 4C 8D 25 E6 1F 76 01"),
    # Material save: ResolveReceiver::ApplyRelocations (lea r13,[ResolveSection::s_pSection] at +0x18, refs +0xA8)
    0x00340E60: bytes.fromhex("48 89 5C 24 18 55 56 41 54 41 55 41 56 48 83 EC 20 48 63 91 90 00 00 00 4C 8D 2D 31 F7 75 01"),
    0x00340E7F: bytes.fromhex("48 8B 41 78 4C 8B D1 4C 89 7C 24 58 4C 8B B9 A8 00 00 00"),
    # Material visibility: CommonRenderModelInstance constructor/destructor and both SetMaterial overloads
    0x002700E0: bytes.fromhex("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05"),
    0x002700F6: bytes.fromhex("48 8B F1 48 89 01 48 8B FA 48 83 C1 08 E8"),
    0x00270108: bytes.fromhex("33 DB 48 89 7E 48"),
    0x00271EA0: bytes.fromhex("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 05"),
    0x00271EB6: bytes.fromhex("48 8B D9 48 89 01 48 8D 05"),
    0x00280460: bytes.fromhex("48 89 4C 24 08 53 55 41 56 48 83 EC 60 48 8B D9 4D 8B F0 48 8B 49 48 8B EA 48 8B 01 FF 50 70 83"),
    0x002805E0: bytes.fromhex("44 88 4C 24 20 53 55 41 54 41 55 48 83 EC 38 4C 8B E9 41 0F B6 D9 48 8B 49 48 49 8B E8 4C 8B E2"),
    # BlendShapePatch guard: render queue removal, ~CommonRenderModel's removal call and mesh loop
    0x002BC750: bytes.fromhex("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 99 00 00 01 00 48 8B F1 48 8B CB 48 8B FA"),
    0x002936D5: bytes.fromhex("49 8D 56 18 E8 72 90 02 00 84 C0"),
    0x002937A2: bytes.fromhex("48 8B 47 38 4C 89 7F 18 48 85 C0 74 43"),
    # Cloth level collision: ClothComponentManager::FinishJobs (the deferred knot probes run after it)
    0x001737A0: bytes.fromhex("40 57 48 83 EC 20 48 8B 0D 4B 1B E4 00 E8 6E CE 07 00 48 8B 0D 3F 1B E4 00 E8 02 B9 07 00 33 FF 39 3D"),
    # InstanceAnim: DecompressFrameWithBoneSet, ObjectZone::GetBoneIndexFromID, EnableForceRebuildTransforms,
    # AnimComponentV2::Init (+0x3B0 graph manager, dtp +0x50 / +0xA0), DecompressFrame time/keys/context setup
    0x0010EF70: bytes.fromhex("48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 98 FE FF FF 48 81 EC 28 02 00"),
    0x00234CC0: bytes.fromhex("48 89 5C 24 08 48 89 74 24 10 48 8B 71 08 45 33 C9 44 0F B7 DA 44 8B 56 30 48 8B 5E 38 45 85 D2"),
    0x00110980: bytes.fromhex("40 53 48 83 EC 20 48 8B 59 08 66 83 89 9E 00 00 00 10"),
    0x00115CC0: bytes.fromhex("40 53 56 48 83 EC 28 48 89 6C 24 40 48 8B D9 48 89 51 08 48 8B F2 48 8B 89 B0 03 00 00 48 89 7C 24 48 49 8B F8 4C 89 83 B0 03 00 00"),
    0x00115D58: bytes.fromhex("48 8B 4F 50 4C 39 B9 A0 00 00 00 74"),
    0x0012B196: bytes.fromhex("F3 41 0F 10 72 60 44 0F BF 40 34"),
    0x0012B1D4: bytes.fromhex("48 8B 41 48 F3 0F 5E D1 0F B7 48 32"),
    0x0012B509: bytes.fromhex("48 8B 55 30 48 8D 8D 70 02 00 00 4C 8B 85 78 02 00 00 48 83 C0 20 48 89 8D A0 01 00 00 49 8B 4E 30 48 89 85 B8 01 00 00"),
    0x0012B5AD: bytes.fromhex("E8 BE 39 FE FF"),
    # ZoneList: ObjectZoneManager::GetZoneName, zonelist.ids loader stores of +0x140 (highest ID) / +0x148 (names)
    0x002378B0: bytes.fromhex("85 C9 75 03 33 C0 C3 8D 41 FF 48 8B 0D 5F 92 81 01 48 63 D0 48 8B 81 48 01 00 00 48 8B 04 D0 C3"),
    0x0024671E: bytes.fromhex("41 89 B5 40 01 00 00"),
    0x00246783: bytes.fromhex("49 89 85 48 01 00 00"),
    # BlendShapePatch: LoadSharedResources 10 MB blendshape TransientGPUBuffer, Alloc, DrawEx call site
    0x002B80B9: bytes.fromhex("41 B8 00 00 A0 00 48 8B D3 48 8B C8 E8 36 AA FD FF"),
    0x002B80D4: bytes.fromhex("48 89 83 98 FB 01 00"),
    0x00296170: bytes.fromhex("4C 8B CA 41 8B C0 F0 0F C1 41 10 0F 1F 44 00 00 8B 41 0C 42 8D 14 00 3B 51 08"),
    0x00278839: bytes.fromhex("E8 32 D9 01 00"),
    
    
    
    
    0x0027BF60: bytes.fromhex("0F B6 41 06 83 F8 15 77 2B 48 8D 15 90 40 D8 FF"),
    0x0030F4E0: bytes.fromhex("83 F9 14 77 6A 48 63 C1 48 8D 15 11 0B CF FF"),
    
    
    
    0x0093EDA0: bytes.fromhex("40 57 48 83 EC 20 F3 0F 10 91 50 01 00 00 0F 57 C0 F3 0F 5C D1"),
    
    0x0090A8FA: bytes.fromhex("C7 83 50 01 00 00 00 00 40 40"),
    
    
    
    
    
    0x00DB4488: bytes.fromhex("D0 3E 91 40 01 00 00 00"),
    0x00913EDF: bytes.fromhex("41 83 38 03"),
    
    
    
    0x008EECF0: bytes.fromhex("40 53 48 83 EC 20 E8 A5 2A C9 FF BA 02 00 00 00"),
    0x008EED4D: bytes.fromhex("C7 44 24 38 03 00 00 00"),
    0x008EED55: bytes.fromhex("E8 16 40 FD FF"),
    
    
    
    
    0x00953B60: bytes.fromhex("48 8B C4 48 83 EC 68 48 89 58 08 44 0F B6 CA 48 89 68 10 48 89 70 18 48 8B F1 4C 89 70 F8"),
    0x00428690: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9"),
    0x004088E0: bytes.fromhex("48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 60 01 00 00"),
    0x002378B0: bytes.fromhex("85 C9 75 03 33 C0 C3 8D 41 FF 48 8B 0D 5F 92 81 01 48 63 D0 48 8B 81 48 01 00 00 48 8B 04 D0 C3"),
    
    
    0x005842F0: bytes.fromhex("0F B6 81 58 04 00 00 C3"),
    0x00584C20: bytes.fromhex("48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 81 EC 60 01 00 00"),

    
    
    
    0x005817A0: bytes.fromhex("48 8B 05 A9 A3 80 02 C3"),
    0x009F98A0: bytes.fromhex("C2 00 00"),
    0x0058F7A0: bytes.fromhex("48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20"),
    0x0048B100: bytes.fromhex("48 8B 05 49 D7 8C 02 C3"),
    0x00488C30: bytes.fromhex("48 8B 01 44 0F B6 C2 33 D2 48 FF A0 90 00 00 00"),
    
    
    
    
    
    
    0x004903A0: bytes.fromhex("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30"),
    0x008A7EE0: bytes.fromhex("48 83 EC 28 48 8B C2 48 8B CA 45 85 C0"),
    0x008A9160: bytes.fromhex("48 89 5C 24 20 48 89 54 24 10 57 48 83 EC 60 48 8B D9"),
    0x008A9198: bytes.fromhex("48 8B 07 48 8B CF 48 89 74 24 70 FF 90 90 00 00 00"),
    0x008A91D0: bytes.fromhex("48 8B 07 4C 8D 84 24 80 00 00 00 8B D3 48 8B CF FF 90 98 00 00 00"),
    
    
    
    
    0x008A8530: bytes.fromhex("40 53 48 83 EC 30 48 C1 EA 20 41 B1 01 45 33 C0 89 51 60 48 8B D9 C6 44 24 20 00 E8 40 ED FF FF"),
    0x008A87D0: bytes.fromhex("48 83 EC 28 0F B6 05 19 5D 57 02 44 8B 41 08 84 C0 74 29 0F B6 05 0B 5D 57 02 BA 09 00 00 00"),
    0x008A8C70: bytes.fromhex("48 89 4C 24 08 53 48 83 EC 70 48 8B D9 BA 03 00 00 00"),
    0x008A8E09: bytes.fromhex("48 8D 0D D0 92 70 00 FF 15 32 9F 45 00 48 8D 53 11 48 8B 48 48 48 8B 01 FF 50 78"),
    0x008A8EA0: bytes.fromhex("48 8D 0D 39 92 70 00 FF 15 9B 9E 45 00 45 8B CE 48 8D 53 11 4D 8B C7 48 8B 48 48 4C 8B 11 41 FF 52 08"),
    
    
    
    
    0x008A8C7D: bytes.fromhex("BA 03 00 00 00 48 8D 0D D7 58 57 02 E8 52 05 B2 FF 83 F8 02"),
    0x003C91E0: bytes.fromhex("48 81 EC 48 01 00 00 45 33 D2 4C 8B CA 4C 8B D9"),
    0x003C9232: bytes.fromhex("41 8B CA FF 15 1D 88 93 00"),
    
    
    
    
    
    0x008A8CF9: bytes.fromhex("4C 8D 7B 20 49 8B CF 4C 89 BC 24 98 00 00 00 FF 15 82 88 45 00"),
    0x008A904D: bytes.fromhex("49 8B CF FF 15 3A 85 45 00"),
    0x008A90A1: bytes.fromhex("49 8B CF FF 15 DE 84 45 00"),
    0x00A41A90: bytes.fromhex("40 55 53 57 48 8D 6C 24 B9 48 81 EC 90 00 00 00"),
    
    
    
    0x00A49FFC: bytes.fromhex("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20"),
    0x00A4888C: bytes.fromhex("48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20"),
    0x00DB1550: bytes.fromhex("30 85 8A 40 01 00 00 00"),
    0x00DB1560: bytes.fromhex("60 91 8A 40 01 00 00 00"),
    0x00DB1590: bytes.fromhex("70 8C 8A 40 01 00 00 00"),
    
    
    0x00DB4480: bytes.fromhex("10 BE 8B 40 01 00 00 00"),
    0x008C2D70: bytes.fromhex("48 83 EC 48 4C 8D 4C 24 68 F3 0F 11 54 24 20"),
    0x00DB49B8: bytes.fromhex("B0 E7 90 40 01 00 00 00"),
    
    
    
    0x00DB49D0: bytes.fromhex("50 06 94 40 01 00 00 00"),
    0x00DB49D8: bytes.fromhex("C0 96 92 40 01 00 00 00"),
    0x00DB49E0: bytes.fromhex("70 65 91 40 01 00 00 00"),
    0x00940650: bytes.fromhex("40 53 48 83 EC 50 48 8B D9 0F 29 7C 24 30"),
    0x00DB4798: bytes.fromhex("90 E6 90 40 01 00 00 00"),
    0x00DB47B0: bytes.fromhex("F0 05 94 40 01 00 00 00"),
    0x00DB47B8: bytes.fromhex("10 BE 8B 40 01 00 00 00"),
    0x00DB47C0: bytes.fromhex("F0 64 91 40 01 00 00 00"),
    0x0090E690: bytes.fromhex("40 53 48 83 EC 20 48 8B D9 E8 32 CA FA FF"),
    0x009405F0: bytes.fromhex("48 83 EC 28 F3 0F 10 81 54 01 00 00"),
    0x00DB4D18: bytes.fromhex("40 C6 90 40 01 00 00 00"),
    0x00DB4D30: bytes.fromhex("20 FC 93 40 01 00 00 00"),
    0x0090C640: bytes.fromhex("48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18"),
    0x0093FC20: bytes.fromhex("40 53 48 83 EC 20 80 B9 03 02 00 00 00"),
    
    
    
    0x006B2980: bytes.fromhex("66 83 7A 02 10 75 3F 80 7A 08 00 48 8B 41 10"),
    0x006B6F60: bytes.fromhex("48 8B 41 10 44 8B 05"),
    
    
    
    0x005D70C0: bytes.fromhex("48 89 5C 24 18 56 57 41 54 41 55 41 57 48 81 EC"),
    0x005D8580: bytes.fromhex("4C 89 4C 24 20 44 89 44 24 18 48 89 54 24 10 48"),
    
    
    
    0x00638A70: bytes.fromhex("48 89 5C 24 20 56 41 55 41 56 48 83 EC 30 49 8B"),
    0x00639790: bytes.fromhex("48 83 B9 C8 02 00 00 00 44 8B CA 74 4B 33 D2 41"),
    0x00646860: bytes.fromhex("48 83 EC 48 44 8B 81 90 00 00 00 4C 8B CA 0F 29"),
    0x00648FF0: bytes.fromhex("88 54 24 10 55 41 54 41 55 41 56 41 57 48 81 EC"),
    0x005DD8C0: bytes.fromhex("4D 85 C0 74 50 44 0F B6 49 40 41 F6 C1 02"),
    0x00639A60: bytes.fromhex("44 88 44 24 18 53 55 56 41 55 41 57 48 83 EC 40"),
    0x00637790: bytes.fromhex("40 53 55 41 56 48 83 EC 30 66 C7 44 24 50 01 01"),
    
    
    0x0063B250: bytes.fromhex("48 89 5C 24 08 48 89 6C 24 18 56 57 41 56 48 83"),
    0x00644E00: bytes.fromhex("48 89 5C 24 18 48 89 74 24 20 41 54 41 56 41 57"),
    0x00644FA0: bytes.fromhex("4C 8B C1 44 0F B6 CA 0F B6 49 40 0F B6 C1 24 01"),
    
    
    0x009F6170: bytes.fromhex("48 8B C4 56 48 81 EC 90 00 00 00 48 89 68 10"),

    
    0x00871930: bytes.fromhex("48 89 5C 24 08 57 48 81 EC C0 01 00 00 8B DA 48 8B F9 83 FA FF 7F 06"),
    0x00449F70: bytes.fromhex("40 53 48 83 EC 20 0F B6 D9"),
    0x00403BF0: bytes.fromhex("40 53 48 83 EC 20 0F B6 D9"),
    0x003DEDE0: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9"),
    0x003DF540: bytes.fromhex("40 53 48 83 EC 60 48 8B DA 48 8D 51 30"),
    0x00852940: bytes.fromhex("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57"),
    0x008531C0: bytes.fromhex("48 8B C1 48 85 C9 74 1A"),

    
    
    
    0x00270050: bytes.fromhex("48 8D 05 F1 B0 AB 00 33 D2 48 89 07 48 8D 4F 28 48 8D 05 41 B1 AB 00 48 89 47 08"),
    0x00280700: bytes.fromhex("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20"),
    
    
    0x002763B0: bytes.fromhex("48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 20"),
    0x003335E0: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 48 8B 01 48 8B FA 48 8B D9 48 3B 50 10 74 1E"),
    0x0027010E: bytes.fromhex("48 8D 05 03 B3 AB 00 48 89 5E 20 48 89 06 48 8B CF 48 8D 05 EA B4 AB 00"),
    0x0027B5CF: bytes.fromhex("4C 8B 57 28 8B C2 48 8D 0C 52 48 C1 E1 05 4C 69 C0 B0 00 00 00"),
    0x00277110: bytes.fromhex("4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 56 57"),
    0x00277350: bytes.fromhex("48 83 EC 48 48 8B 05 E5 CA 7D 01 45 33 C9 44 89 4C 24 30"),
    0x00297620: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 30 8B B9 18 01 00 00"),
    0x0024CF30: bytes.fromhex("48 89 6C 24 18 57 48 83 EC 20 48 8B F9"),
    
    0x0024CBD0: bytes.fromhex("40 53 48 83 EC 30 48 8B D9 0F 29 74 24 20"),

    
    
    
    0x0022A0D0: bytes.fromhex("40 55 53 57 48 8D 6C 24 F0 48 81 EC 10 01 00 00 48 8B F9 48 8B DA 48 8B 49 08"),
    0x0072831F: bytes.fromhex("0F B7 4D E2 0F 57 C0 8B 43 38 41 83 CF FF C6 43 58 01 BA 00 C2 00 00 66 0F 6E 8F C4 02 00 00 66 23 CA F3 0F 10 53 60 BA 00 02 00 00 F3 0F 10 5B 5C 66 0B CA 0F 5B C9 48 8D 53 50 66 83 C9 10 66 83 C9 02 41 B0 01 66 89 4D E2 48"),
    0x00728420: bytes.fromhex("48 8D 54 24 30 48 8B CF E8 A3 1C B0 FF EB 3E"),
    0x00D0CCE8: bytes.fromhex("00 00 80 BF"),
    0x0022CCE0: bytes.fromhex("40 57 48 81 EC A0 00 00 00 48 8B F9 48 8B 0D 5D EE B5 02 48 8B 01 FF 90 00 01 00 00"),
    0x0023E3B5: bytes.fromhex("48 8B CD E8 23 E9 FE FF EB 19"),
    0x00672260: bytes.fromhex("48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 50 48 8B 79 10 48 8B D9"),
    0x006723A8: bytes.fromhex("F3 0F 10 83 54 03 00 00 0F 57 F6 0F 2E C6"),
    0x006723C3: bytes.fromhex("89 83 58 03 00 00 F3 0F 10 BB 50 03 00 00"),
    
    
    0x00672831: bytes.fromhex("44 0F B6 BF 4D 03 00 00 F3 0F 10 BF 50 03 00 00 F3 0F 10 B7 54 03 00 00"),

    
    
    
    0x0044B270: bytes.fromhex("48 8B 05 B9 C1 90 02 8B 40 30 C3"),
    0x008B42D0: bytes.fromhex("48 8B 05 49 A4 56 02 0F B6 40 18 C3"),
    0x008B3890: bytes.fromhex("84 C9 78 23 4C 8B 05 85 AE 56 02 0F BE C1 41 0F B6 50 18 3B C2 7D 10 48 0F BE C1 48 69 C0 A8 00 00 00 49 03 40 10 C3"),
    0x008ECEB0: bytes.fromhex("40 53 48 83 EC 20 0F B6 D9 48 8B 0D 90 EC 49 02 48 8B 01 FF 50 18 48 8B 0D AB EC 49 02 44 0F B6 C3 48 8B D0 48 83 C4 20 5B"),
    0x00581E20: bytes.fromhex("48 8B 81 E8 60 00 00 48 85 C0 74 08 48 8B 80 A8 31 00 00 C3"),
    0x0058D380: bytes.fromhex("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 80 B9 F0 60 00 00 00 41 0F B6 F9 49 8B F0 48 8B EA 48 8B D9"),
    
    
    
    0x0058D3C1: bytes.fromhex("80 8B 18 60 00 00 02 40 88 BB 19 60 00 00 48 8B 83 38 03 00 00 40 88 78 31"),
    
    
    
    0x0072E0B4: bytes.fromhex("4C 8D 47 30 44 0F B6 CD 48 8D 57 10 48 8B C8 E8 B8 F2 E5 FF"),
    0x0093C2DE: bytes.fromhex("4C 8D 43 30 44 0F B6 CE 49 8B CE 48 8D 50 10 E8 8E 10 C5 FF"),
    
    
    0x0093C2F7: bytes.fromhex("83 F9 02 0F 85 23 02 00 00 48 8B 0D D1 26 4E 02 E8 24 83 B3 FF 48 8B 8F 40 02 00 00 33 D2 E8 B6 09 AA FF 4C 8B F0 48 85 C0"),
    0x0093C326: bytes.fromhex("48 8B 8F 70 02 00 00 4C 8D 87 C0 01 00 00 B2 01 0F B7 49 04 E8 31 B0 90 FF"),
    0x00581890: bytes.fromhex("0F B6 91 19 60 00 00 E9 44 FF FF FF"),
    0x0023BC50: bytes.fromhex("48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 45 8B F1 49 8B F8 0F B6 F2 8B D9 85 C9 75 04 32 C0 EB 65"),
    0x00247370: bytes.fromhex("48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B E8 0F B6 F2 8B F9 85 C9 75 12 32 C0"),
    
    
    0x0043E660: bytes.fromhex("48 83 EC 38 F7 05 6E 92 91 02 00 00 00 10 0F 29 74 24 20 0F 85 3F 02 00 00"),
    0x00121030: bytes.fromhex("48 8B 51 68 48 B8 00 00 00 00 FF FF FF FF 48 85 D0"),
    0x00602D30: bytes.fromhex("48 8B 81 B0 02 00 00 48 85 C0 74 09 48 8B 40 08 0F B6 40 2D C3"),

    
    
    
    
    
    
    0x00449F9E: bytes.fromhex("48 8B 80 E8 60 00 00 48 83 C4 20 5B C3"),
    0x00408299: bytes.fromhex("48 8B 88 E8 60 00 00 48 85 C9 74 29 E8 76 EC 41 00"),
    0x00443A46: bytes.fromhex("48 8B 88 E8 60 00 00 48 85 C9 74 15 E8 D9 7A 3E 00"),
    
    
    0x008A28F0: bytes.fromhex("48 83 EC 28 48 8B 41 40 48 85 C0 74 2A 8B 0D 8D 81 85 00 48 8B 80 D8 00 00 00"),
    0x008A291F: bytes.fromhex("8B 40 44 48 83 C4 28 C3"),
    0x00826F20: bytes.fromhex("48 83 EC 28 E8 E7 2C 00 00 48 85 C0 74 0C 48 8B 80 58 62 00 00 48 83 C4 28 C3"),
    0x008A6150: bytes.fromhex("40 53 48 83 EC 20 48 8B 41 40 8B DA 48 85 C0 74 2C 8B 0D 29 49 85 00 48 8B 80 D8 00 00 00"),
    0x008A6183: bytes.fromhex("39 58 44 0F 42 58 44 29 58 44"),
    0x008A3090: bytes.fromhex("40 53 48 83 EC 30 48 83 3A 00 48 8B DA 74 42 48 8B 41 40 48 85 C0 74 39"),
    
    
    
    
    
    0x008A30CA: bytes.fromhex("0F B7 53 08 45 33 C9 45 33 C0 C6 44 24 20 00 48 8B C8"),
    
    0x0044D5A0: bytes.fromhex("48 8B 15 F9 9E 90 02 48 85 D2 74 18 85 C9 78 14 3B 4A 20 73 0F 48 63 C1 48 69 C0 C0 01 00 00"),
    
    0x0044D600: bytes.fromhex("8B C8 48 69 D1 C0 01 00 00 48 03 D3 39 7A 04"),
    0x0044D680: bytes.fromhex("48 8B 48 28 48 8B 8C 39 88 01 00 00"),
    
    
    0x0082B530: bytes.fromhex("48 83 EC 28 E8 D7 E6 FF FF 48 85 C0 74 0B 48 05 88 03 00 00 48 83 C4 28 C3"),
    
    
    0x0082E8C0: bytes.fromhex("40 55 56 41 54 41 55 41 56 48 83 EC 30 48 8B 01 45 0F B6 E1 45 0F B6 E8 0F B7 F2 4C 8B F1"),
    0x008470F0: bytes.fromhex("48 83 EC 38 B8 FF 03 00 00 66 3B D0 0F 87 ED 00 00 00 0F B7 C2 48 89 7C 24 30 0F B7 FA 8B D7"),
    0x0084710F: bytes.fromhex("C6 44 08 08 00"),

    
    
    0x005A43DE: bytes.fromhex("48 8D 05 73 65 7B 00 48 89 03 48 8D 8B B8 02 00 00 48 8D 05 92 65 7B 00 48 89 AB B0 02 00 00 48 89 43 18"),
    
    
    0x005A4583: bytes.fromhex("48 8D 4F 28 48 89 07 E8 01 5B D7 FF 48 89 BB A8 03 00 00"),
    
    
    0x005ABDB3: bytes.fromhex("0F B7 5F 0C 48 8B D5 48 8B 8E A8 03 00 00 66 D1 EB 48 83 C1 28"),
    
    
    0x005A8932: bytes.fromhex("41 8D 50 10 FF 90 90 00 00 00 48 8B C8 48 85 C0 74 1E 48 89 28 89 70 08 66 44 89 70 0C EB 14 48 85 C0 74 8C 48 83 C0 0C"),
    
    
    0x00604CC0: bytes.fromhex("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 41 8B F9"),
    0x005D0E30: bytes.fromhex("40 56 48 83 EC 30 48 83 B9 C8 02 00 00 00 48 8B F1 44 8B CA"),
    0x0063BDD0: bytes.fromhex("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 40 48 8B D9 41 0F B6 F8"),
    
    
    
    
    
    0x00656D70: bytes.fromhex("48 83 EC 28 48 8B 05 0D 32 74 02 48 85 C0 75 30"),
    0x0065A220: bytes.fromhex("48 89 5C 24 18 57 48 83 EC 30 48 8B F9 E8 CE 0E E3 FF"),
    0x0065A8E0: bytes.fromhex("8B 91 80 04 00 00 85 D2 74 28 8B 05 7C D0 6F 02"),
    0x006638A0: bytes.fromhex("48 89 6C 24 20 56 48 83 EC 30 48 8B F1 89 54 24 20"),
    0x00653130: bytes.fromhex("44 89 44 24 18 89 54 24 10 48 89 4C 24 08 53 55 56 57 48 83 EC 58"),
    0x00661DE0: bytes.fromhex("48 83 EC 48 E8 07 59 FF FF 48 85 C0"),
    0x006576F0: bytes.fromhex("48 83 B9 28 04 00 00 00 44 8B C2 74 77 33 D2 41 8B C0"),
    0x00656DC0: bytes.fromhex("4C 8B 81 D8 03 00 00 8B C2 48 8D 14 40 49 8B 40 30 8B 14 90"),
    0x00216300: bytes.fromhex("40 53 48 83 EC 20 48 8B D9 48 8B 49 08 48 8B 01"),
    0x000832D0: bytes.fromhex("48 85 D2 74 03 C6 02 01 83 F9 FF 75 10"),
    0x005D5110: bytes.fromhex("4C 8B A1 08 03 00 00 C6 84 24 98 00 00 00 01 41 39 6C 24 68"),
    0x005D5140: bytes.fromhex("49 8B 44 24 60 45 0F B7 04 06"),
    
    
    0x0044634F: bytes.fromhex("48 8B 15 DA 10 91 02 33 ED 48 8B F1 0F B7 DD 66 3B 6A 20"),
    0x00446370: bytes.fromhex("48 8B 4A 28 4C 8D 05 1D 30 8E 00 0F B7 C3 48 6B F8 68"),

    
    
    
    
    0x008531CE: bytes.fromhex("39 88 D0 00 00 00 76 0C 48 8B 80 D8 00 00 00 48 8B 04 C8 C3"),
    0x0016EE90: bytes.fromhex("40 53 48 83 EC 20 48 8B D9 E8"),
    0x0016EE9E: bytes.fromhex("48 8D 05 B3 B6 BA 00 48 89 03 33 C0 89 43 18 48 89 43 20 48 89 43 28 89 43 30"),
    0x001779E0: bytes.fromhex("40 56 48 83 EC 60 80 B9 02 01 00 00 00 44 8B CA 48 8B F1 0F 85"),
    0x00177AFF: bytes.fromhex("48 6B E9 70 48 03 68 28 83 7D 40 00"),
    0x00177B89: bytes.fromhex("F3 0F 10 45 00 F3 0F 59 C7 48 8B 0C 07 F3 0F 11 41 4C"),
    0x00177BA3: bytes.fromhex("8B 45 04 89 81 A8 00 00 00"),
    0x00177CA1: bytes.fromhex("0F B6 45 1C C0 E0 05 32 81 D8 00 00 00 24 20 30 81 D8 00 00 00"),
    
    
    
    
    
    0x00176CB0: bytes.fromhex("40 53 48 83 EC 30 8B 41 68 48 8B D9 0F AF 41 64 0F 29 74 24 20 F3 0F 10 71 48 66 0F 6E C0 0F 5B"),
    0x001BB350: bytes.fromhex("48 83 EC 48 41 0F 28 01 0F 57 C9 41 0F 5C 00 0F 5C C8 0F 5F C8 0F C2 0D"),
    
    
    
    0x007CD1EA: bytes.fromhex("48 8B 81 A8 02 00 00 48 8B 88 E0 1A 00 00 48 85 C9 74 53 F3 0F 10 47 18 0F 2F 80 E8 1A 00 00"),
    0x0070D280: bytes.fromhex("48 8B C4 48 89 58 18 55 56 57 41 56 41 57 48 8D 68 B8 48 81 EC 20 01 00 00 4C 8B 71 10"),
    0x0070D335: bytes.fromhex("BA AA 01 00 00 4C 8D 45 50 45 33 C9 E8 3A EA FC FF"),
    0x006DBD80: bytes.fromhex("4D 8B D0 4C 8B C1 48 0F BF CA 48 C1 E1 07 66 90 42 80 BC 01 45 1C 00 00 00"),

    
    0x00277C40: bytes.fromhex("41 0F B6 96 84 00 00 00 0F 28 DF F3 41 0F 10 86 88 00 00 00 0F B6 CA 48 8B 45 08 80 E2 01"),
    0x00277CF7: bytes.fromhex("F3 41 0F 10 46 6C C0 E9 03 80 E1 01 48 89 7D D0"),

    
    
    
    0x00222A1E: bytes.fromhex("48 8D 05 4B 66 B0 00 0F 57 C0 48 89 03 33 C0 0F"),
    0x0024D1A0: bytes.fromhex("48 83 79 48 00 74 49 39 51 64 74 44 48 8B 41 50 44 0F B7 40 14 41 3B D0"),
    0x0024D200: bytes.fromhex("39 51 6C 74 4D 0F 28 C2 0F 57 DB 0F 57 D2 F3 0F 10 D8 F3 0F 5F DA 0F 28"),
    0x0024C8D0: bytes.fromhex("40 53 55 57 48 81 EC 80 01 00 00 48 8B FA 48 8B D9 48 85 D2 75 08 48 8B"),
    
    
    
    0x003435F0: bytes.fromhex("81 F9 00 30 11 00 73 21 8B C1 48 8D 0D"),
    0x00270208: bytes.fromhex("48 8D 05 51 B4 AB 00 48 89 01 0F 57 C0 48 8B 44"),
    0x002701F9: bytes.fromhex("4C 89 41 10 45 33 C0 48 89 51 08 4C 89 49 18"),
    0x002D4127: bytes.fromhex("48 8D 05 DA A5 A5 00 48 89 01 49 8B 41 10 48 89"),

    
    
    
    
    
    0x00342CD0: bytes.fromhex("40 53 41 56 48 83 EC 38 33 DB 4C 8B F1 39 59 04"),
    
    
    0x00342D00: bytes.fromhex("49 8B 46 08 48 8B 6C 07 60 48 8D 34 07 48 85 ED 74 36 4C 8B 7D 10 48 8D 4C 24 20 48 8B D6"),
    0x00342D23: bytes.fromhex("0F 28 46 30 41 0F 29 07 0F 28 44 24 20 41 0F 29 47 10 0F B6 45 0C 3C FF 75 04 33 C0 EB 03 0F B6 C0 41 89 47 20"),
    0x001E3A50: bytes.fromhex("40 53 48 83 EC 30 48 8B D9 41 B8 15 00 00 00 48 8D 4C 24 20"),
    0x0034697D: bytes.fromhex("3B 05 DD 9E 75 01 75 0D 48 8B CB E8 43 C3 FF FF"),

    
    
    0x003F2580: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 BE 99 FD FF 33 FF 48 C7 43 48 60 00 00 00 48 8D 05 9D 2A 95 00"),
    0x003F259C: bytes.fromhex("48 8D 05 9D 2A 95 00 48 89 7B 38 48 8D 8B A0 00 00 00 48 89 03"),
    
    
    0x00118EDD: bytes.fromhex("48 8B 05 5C E5 C3 02 48 8B 58 10 48 8B 43 38 66 83 88 38 01 00 00 02"),
    
    
    
    0x0017B0E0: bytes.fromhex("48 8B C4 57 41 54 41 55 48 81 EC C0 00 00 00 48 8B 79 10 4D 8B E1 0F 29 70 C8 4D 8B E8"),
    0x0017B112: bytes.fromhex("0F B7 41 44 44 0F 28 0D 92 94 F8 00 44 0F 28 15 9A 94 F8 00 66 85 C0"),
    0x0017B15E: bytes.fromhex("0F BF 47 04 0F 57 C0 66 0F 6E C8 0F BF 47 02 0F 5B C9 66 0F 6E D0 0F BF 07 48 83 C7 06"),
    0x0017B398: bytes.fromhex("0F B7 4F 04 E8 FF B6 24 00 0F B7 4F 02 0F 28 F8 E8 F3 B6 24 00 0F B7 0F"),

    
    
    
    0x003CC04C: bytes.fromhex("48 8D 05 7D 79 97 00 48 89 79 08 48 8B D9 89 79 10 33 D2 48 89 01 66 89 79 18"),

    
    
    
    
    0x0043D425: bytes.fromhex("48 8D 05 C4 DC 90 00 48 89 03 48 89 11 C7 41 08 08 00 00 00 48 89 93 98 03 00 00"),
    0x003F1999: bytes.fromhex("48 8D 05 10 32 95 00 48 89 03 48 89 AB 10 03 00 00 48 89 AB 18 03 00 00 40 88 AB 20 03 00 00"),
    0x003F1AA0: bytes.fromhex("48 C1 E2 06 41 FF 91 90 00 00 00 48 63 8B 58 03 00 00 33 D2 48 89 83 68 03 00 00 4C 8D 04 49"),
    0x003F1ACD: bytes.fromhex("48 89 AB 28 03 00 00 48 89 AB 30 03 00 00 48 89 AB 38 03 00 00 48 89 AB 40 03 00 00 48 89 AB 48 03 00 00 48 89 AB 50 03 00 00"),
    
    
    
    0x003F1B28: bytes.fromhex("48 8B 74 24 40 48 8B C3 48 89 AB 5C 03 00 00"),
    
    
    0x0047D055: bytes.fromhex("49 8B 42 48 48 8B 79 08 48 85 C0 74 06 48 8B 58 70 EB 08 49 8B 42 40 48 8B 58 68"),
    
    
    0x003F6BD9: bytes.fromhex("48 8B 43 40 C6 40 33 07 49 8B 07 48 8B 48 08 48 8B 43 40 48 81 C1 90 00 00 00 48 89 48 60 48 8B 43 40 48 8B 48 60 C7 01 02 00 00 00"),
    
    
    
    
    0x004045C0: bytes.fromhex("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 F7 41 50 00 00 08 00 49 8B F1 49 8B E8 8B FA 48 8B D9"),
    0x004046C0: bytes.fromhex("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC A0 00 00 00 49 63 F8 48 8B F2 48 8B D9 45 85 C0 78 09 3B 79 58"),
    0x0040476F: bytes.fromhex("48 8B 43 40 48 FF C7 48 03 FF F6 43 54 01 48 8B 68 60 0F 28 44 FD 00 66 0F 7F 06"),

    
    
    
    
    0x0053C2E0: bytes.fromhex("48 8B D1 48 85 C9 75 05 33 C9 8B C1 C3 81 FA 30 75 00 00 73 F3 48 8D 0D D4 27 FB 01 8B C2 48 8D 04 C0 48 C1 E0 05 48 03 C1 33 C9 48 39 90 18 01 00 00 48 0F 45 C1 C3"),
    
    
    
    
    
    0x001F9FD0: bytes.fromhex("48 8B 41 40 48 8B 88 80 00 00 00 48 8B 41 08 8B 40 50 C3"),
    0x00333760: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 0F 28 02 48 8B FA 0F C2 41 10 04"),
    0x003ED760: bytes.fromhex("40 53 48 83 EC 70 48 8B D9 48 8D 4C 24 30"),
    
    
    
    0x003D6600: bytes.fromhex("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 0F B6 F2"),
    
    
    
    0x005DD9D0: bytes.fromhex("48 89 5C 24 08 55 48 8B EC 48 83 EC 70 45 33 DB"),
    0x003CD7A0: bytes.fromhex("48 8B 01 45 33 C0 48 3B 41 10 74 10"),
    0x003CD7E0: bytes.fromhex("48 8B 01 48 3B 41 10 74 33 4C 8B 10"),
    0x003E7280: bytes.fromhex("48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 56 41 57 48 83 EC 50"),
    0x003EA490: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 50 48 8B D9 8B FA"),
    0x005D230D: bytes.fromhex("8B 2D D5 9E 7B 02"),
    
    0x003D98E0: bytes.fromhex("44 0F B6 C2 4C 8D 0D 1D 16 C3 00 33 D2 E9 6E F9 FF FF"),
    0x0023B040: bytes.fromhex("85 C9 74 3E 4C 8B 05 D5 5A 81 01 49 83 78 38 00"),
    0x003CC7C0: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 80 61 4C 07 48 8D 05 D3 94 95 00"),
    
    0x003CC82D: bytes.fromhex("89 93 18 01 00 00 33 D2 48 89 BB 10 01 00 00 44 89 43 44 44 89 43 48"),
    0x003CD260: bytes.fromhex("40 53 48 83 EC 20 48 8D 05 53 8A 95 00 48 8B D9 48 89 01"),
    0x003D0BC0: bytes.fromhex("40 55 56 41 56 41 57 48 8D AC 24 78 F8 FF FF 48 81 EC 88 08 00 00 44 8B 79 48"),
    0x003ED730: bytes.fromhex("80 49 4C 10 0F 28 02 66 0F 7F 41 60 C3"),
    0x003ED750: bytes.fromhex("80 49 4C 20 0F 28 02 66 0F 7F 41 70 C3"),
    0x003D0C6B: bytes.fromhex("44 0F B7 B1 B0 00 00 00 45 8B FE"),
    0x003D0CD8: bytes.fromhex("8B 8E 18 01 00 00 41 B9 03 00 00 00 45 33 C0 33 D2 E8 62 AF E6 FF"),
    
    
    
    0x00199D94: bytes.fromhex("48 8B 0D 15 AD F6 00 8B DA 48 8B 01 FF 50 58 48 8B 0D 3E AD F6 00 48 85 C9 74 10 48"),
    0x00197A60: bytes.fromhex("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 DB 48 8B F2 48 8B F9 39 99 98 06 00 00 76 24 90 4C 8D 04 5B 48 8B D6 4D 03 C0 4A 8B 4C C7 08 48 8B 01 FF 50 10 84 C0 75 0D FF C3 3B 9F 98 06 00 00 72 DD 83 CB FF 48 8B 74 24 38 C1 EB 1F 80 F3 01 0F B6 C3 48 8B 5C 24 30 48 83 C4 20 5F C3"),

    
    
    
    0x00487200: bytes.fromhex("48 83 EC 28 B9 E0 00 00 00 E8 C2 DF F3 FF 48 85 C0 74 14 48 8B C8 E8 35 D0 FF FF 48 89 05 3E 16 8D 02"),
    0x004859F0: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 81 FA FF 00 00 00"),
    0x002422A0: bytes.fromhex("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 83 3D A9 EA 80 01 00"),
    0x00485A70: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 8B CA 48 C1 E1 04 48 03 4F 08 80 39 00 75 68"),
    0x00487460: bytes.fromhex("48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 8B CA 48 C1 E1 04 48 03 4F 08 80 39 00 74 67"),
    0x004998C0: bytes.fromhex("83 FA FF 0F 84 1B 01 00 00 48 89 5C 24 08 57 48 83 EC 50 0F 29 74 24 40 48 8B F9 0F 29 7C 24 30"),

    
    0x00401FD0: bytes.fromhex("48 8D 05 E9 59 95 02 C3"),
    0x00405D30: bytes.fromhex("48 8B 01 F3 0F 10 80 DC 00 00 00 C3"),
    0x00428C00: bytes.fromhex("48 8B 01 0F 28 D9 F3 0F 10 80 DC 00 00 00"),
    0x001FE3A0: bytes.fromhex("48 83 EC 28 84 D2 74 30"),
    0x001FCF00: bytes.fromhex("80 3D 5E AA B5 02 02 0F 94 C0 C3"),

    
    0x0014A590: bytes.fromhex("40 53 48 83 EC 20 65 48 8B 04 25 58 00 00 00"),
    0x009EF120: bytes.fromhex("33 D2 48 83 C1 20 44 8D 42 40"),

    
    
    
    0x0014E6C0: bytes.fromhex("48 83 EC 28 48 8B 05 65 37 FB 00 48 85 C0"),
    0x0014A740: bytes.fromhex("48 83 B9 98 00 00 00 00 4C 8B C9 44 8B C2"),
    
    
    0x0014A752: bytes.fromhex("41 8B C0 48 F7 B1 A0 00 00 00 48 8B 81 88 00 00 00 48 8B 04 D0"),
    
    
    0x00745AB6: bytes.fromhex("8B 97 58 32 00 00 E8 7F 4C A0 FF"),

    
    
    0x0013E1F0: bytes.fromhex("0F 28 02 66 0F 7F 01 0F 28 4A 10 66 0F 7F 49 10"),
    0x0013E236: bytes.fromhex("0F 28 82 80 00 00 00 66 0F 7F 81 80 00 00 00"),
    0x0013E245: bytes.fromhex("0F 28 8A A0 00 00 00 0F 29 89 A0 00 00 00 8B 82 B0 00 00 00 89 81 B0 00 00 00"),

    
    
    0x00141BD0: bytes.fromhex("40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 30 02 00 00 48"),
    0x0014204E: bytes.fromhex("48 8D 8E C0 01 00 00 88 85 31 01 00 00 E8 90 C1 FF FF"),
    0x00142070: bytes.fromhex("48 8D 4E 20 48 8D 55 20 E8 73 C1 FF FF"),

    
    0x0087B7C2: bytes.fromhex("48 8B D0 48 8D 8B 10 04 00 00 E8 1F 2A 8C FF 45 84 F6"),

    
    
    
    0x0056B5A0: bytes.fromhex("48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B F9 48 63 F2"),
    0x0068BC40: bytes.fromhex("E8 9B 18 EE FF"),
    0x0068BCC0: bytes.fromhex("E8 1B 18 EE FF"),
    0x0068BD3B: bytes.fromhex("E8 A0 17 EE FF"),
    0x0068BDA9: bytes.fromhex("E8 32 17 EE FF"),
    0x0068BDC0: bytes.fromhex("E8 1B 17 EE FF"),
    0x0068BE29: bytes.fromhex("E8 B2 16 EE FF"),
    0x0083EF20: bytes.fromhex("40 53 48 81 EC C0 00 00 00 48 8B D9 0F 29 BC 24 90 00 00 00 48 8B 09 0F 28 FA"),
    0x0082E300: bytes.fromhex("40 53 48 83 EC 70 48 8B 01 48 8B DA 4C 8B 88 F0 00 00 00 41 0F 28 41 20"),

}

def main(path: str) -> int:
    data = Path(path).read_bytes()
    if data[:2] != b"MZ":
        raise SystemExit("Not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe+4] != b"PE\0\0":
        raise SystemExit("Bad PE signature")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    sec0 = pe + 24 + optsz
    sections = []
    for i in range(nsec):
        off = sec0 + i * 40
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, off + 8)
        sections.append((va, max(vsize, rawsize), raw))

    def at(rva, count):
        for va, size, raw in sections:
            if va <= rva < va + size:
                o = raw + (rva - va)
                return data[o:o+count]
        return b""

    ok = True
    for rva, sig in TESTS.items():
        good = at(rva, len(sig)) == sig
        print(f"0x{rva:08X}: {'PASS' if good else 'FAIL'}")
        ok &= good

    
    
    for label, insn_rva, opcode, insn_len, disp_off, expected in (
        ("Chapter Select globalInfo pointer", 0x00953B8B, bytes.fromhex("48 8B 05"), 7, 3, 0x02D57478),
        ("Chapter Select RunnableManager pointer", 0x00953BFC, bytes.fromhex("48 8B 0D"), 7, 3, 0x02D35B00),
        ("Chapter Select NetManager pointer", 0x00953C1D, bytes.fromhex("48 8B 3D"), 7, 3, 0x02D8BB50),
        ("Chapter Select g_hardLoadForceSave", 0x00953C24, bytes.fromhex("C6 05"), 7, 2, 0x02D8C1DA),
    ):
        raw = at(insn_rva, insn_len)
        target = None
        if len(raw) == insn_len and raw[:len(opcode)] == opcode:
            disp = struct.unpack_from("<i", raw, disp_off)[0]
            target = insn_rva + insn_len + disp
        good = target == expected
        print(f"{label}: {'PASS' if good else 'FAIL'}" +
              (f" (0x{target:08X})" if target is not None else ""))
        ok &= good

    
    
    chapter_layout = at(0x009534B4, 13)
    chapter_layout_good = chapter_layout == bytes.fromhex(
        "8B B8 E8 27 00 00 48 8B 98 F0 27 00 00")
    print(f"Chapter Select release-list layout +0x27E8/+0x27F0: "
          f"{'PASS' if chapter_layout_good else 'FAIL'}")
    ok &= chapter_layout_good

    
    
    
    store_rva = 0x0048721B
    store = at(store_rva, 7)
    target = None
    if len(store) == 7 and store[:3] == bytes.fromhex("48 89 05"):
        disp = struct.unpack_from("<i", store, 3)[0]
        target = store_rva + 7 + disp
    singleton_good = target == 0x02D58860
    print(f"WeatherManager singleton target: {'PASS' if singleton_good else 'FAIL'}"
          + (f" (0x{target:08X})" if target is not None else ""))
    ok &= singleton_good

    
    
    
    
    traits_load_rva = 0x0044D5A0
    traits_load = at(traits_load_rva, 7)
    traits_singleton = None
    if len(traits_load) == 7 and traits_load[:3] == bytes.fromhex("48 8B 15"):
        disp = struct.unpack_from("<i", traits_load, 3)[0]
        traits_singleton = traits_load_rva + 7 + disp
    traits_singleton_good = traits_singleton == 0x02D574A0
    print(f"PlayerTraits singleton/table target: {'PASS' if traits_singleton_good else 'FAIL'}"
          + (f" (0x{traits_singleton:08X})" if traits_singleton is not None else ""))
    ok &= traits_singleton_good

    
    
    
    inv_ctor_rva = 0x005A43DE
    inv_lea = at(inv_ctor_rva, 7)
    inv_vtable = None
    if len(inv_lea) == 7 and inv_lea[:3] == bytes.fromhex("48 8D 05"):
        disp = struct.unpack_from("<i", inv_lea, 3)[0]
        inv_vtable = inv_ctor_rva + 7 + disp
    inv_vtable_good = inv_vtable == 0x00D5A958
    print(f"InventoryComponent primary vtable target: {'PASS' if inv_vtable_good else 'FAIL'}"
          + (f" (0x{inv_vtable:08X})" if inv_vtable is not None else ""))
    ok &= inv_vtable_good

    drawable_ctor_rva = 0x003CC04C
    drawable_lea = at(drawable_ctor_rva, 7)
    drawable_vtable = None
    if len(drawable_lea) == 7 and drawable_lea[:3] == bytes.fromhex("48 8D 05"):
        disp = struct.unpack_from("<i", drawable_lea, 3)[0]
        drawable_vtable = drawable_ctor_rva + 7 + disp
    drawable_good = drawable_vtable == 0x00D439D0
    print(f"DrawableComponent primary vtable target: {'PASS' if drawable_good else 'FAIL'}"
          + (f" (0x{drawable_vtable:08X})" if drawable_vtable is not None else ""))
    ok &= drawable_good

    markup_ctor_rva = 0x0043D425
    markup_lea = at(markup_ctor_rva, 7)
    markup_vtable = None
    if len(markup_lea) == 7 and markup_lea[:3] == bytes.fromhex("48 8D 05"):
        disp = struct.unpack_from("<i", markup_lea, 3)[0]
        markup_vtable = markup_ctor_rva + 7 + disp
    markup_vtable_good = markup_vtable == 0x00D4B0F0
    print(f"TRMarkupManager primary vtable target: {'PASS' if markup_vtable_good else 'FAIL'}"
          + (f" (0x{markup_vtable:08X})" if markup_vtable is not None else ""))
    ok &= markup_vtable_good

    markup_store_rva = 0x0043D454
    markup_store = at(markup_store_rva, 7)
    markup_singleton = None
    if len(markup_store) == 7 and markup_store[:3] == bytes.fromhex("48 89 1D"):
        disp = struct.unpack_from("<i", markup_store, 3)[0]
        markup_singleton = markup_store_rva + 7 + disp
    markup_singleton_good = markup_singleton == 0x02D57400
    print(f"TRMarkupManager singleton target: {'PASS' if markup_singleton_good else 'FAIL'}"
          + (f" (0x{markup_singleton:08X})" if markup_singleton is not None else ""))
    ok &= markup_singleton_good

    
    
    for label, ctor_rva, expected in (
        ("CommonMaterial primary vtable", 0x00270050, 0x00D2B148),
        ("CommonRenderModelInstance primary vtable", 0x0027010E, 0x00D2B418),
    ):
        lea = at(ctor_rva, 7)
        target = None
        if len(lea) == 7 and lea[:3] == bytes.fromhex("48 8D 05"):
            disp = struct.unpack_from("<i", lea, 3)[0]
            target = ctor_rva + 7 + disp
        good = target == expected
        print(f"{label}: {'PASS' if good else 'FAIL'}" + (f" (0x{target:08X})" if target is not None else ""))
        ok &= good

    
    
    for label, ctor_rva, expected in (
        ("DynamicHairComponent primary vtable", 0x00222A1E, 0x00D29070),
        ("DynamicHairDrawable primary vtable", 0x00270208, 0x00D2B660),
        ("HairComputeDrawable primary vtable", 0x002D4127, 0x00D2E708),
    ):
        lea = at(ctor_rva, 7)
        target = None
        if len(lea) == 7 and lea[:3] == bytes.fromhex("48 8D 05"):
            disp = struct.unpack_from("<i", lea, 3)[0]
            target = ctor_rva + 7 + disp
        good = target == expected
        print(f"{label}: {'PASS' if good else 'FAIL'}" + (f" (0x{target:08X})" if target is not None else ""))
        ok &= good

    return 0 if ok else 1

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_retail_offsets.py ROTTR.exe")
    raise SystemExit(main(sys.argv[1]))
