
/*
**---------------------------------------------------------------------------
** OpenChasm - Free software reconstruction of Chasm: The Rift game
** Copyright (C) 2013 Alexey Lysiuk
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program.  If not, see <http://www.gnu.org/licenses/>.
**---------------------------------------------------------------------------
*/

#include "cspbio.h"

namespace CSPBIO
{

namespace
{

bool     s_isInternal;   // Internal

OC::Path s_lastFilePath; // LastFName

OC::Path s_addonPath;    // AddonPath
bool     s_isUserMaps;   // UserMaps


class FileTableEntry
{
public:
    FileTableEntry()
    : m_size(0)
    , m_offset(0)
    {
    }

    const OC::String&  filename() const { return m_filename; }

    const std::streamsize  size() const { return m_size;   }
    const std::streamoff offset() const { return m_offset; }

    void read(OC::File& stream);

private:
    OC::String      m_filename;

    std::streamsize m_size;
    std::streamoff  m_offset;
};


void FileTableEntry::read(OC::File& stream)
{
    m_filename = stream.readPascalString(12); // filename in 8.3 format

    m_size   = stream.readBinary<Sint32>();
    m_offset = stream.readBinary<Sint32>();
}


typedef OC::Array<FileTableEntry> FileTable;
FileTable s_fileTable; // FileTable

} // unnamed namespace


// ===========================================================================


class EmbeddedFileBuffer : public boost::filesystem::filebuf
{
private:
    typedef boost::filesystem::filebuf Base;

public:
    explicit EmbeddedFileBuffer(const OC::Path& filePath);

protected:
    virtual pos_type seekoff(off_type off, std::ios::seekdir way,
        std::ios::openmode which = std::ios::in | std::ios::out);
    virtual pos_type seekpos(pos_type pos,
        std::ios::openmode which = std::ios::in | std::ios::out);

private:
    std::streamsize m_size;
    std::streamoff  m_offset;

    bool m_isEmbedded;

};


EmbeddedFileBuffer::EmbeddedFileBuffer(const OC::Path& path)
: m_size(0)
, m_offset(0)
, m_isEmbedded(false)
{
    s_lastFilePath = path;
    
    if (s_isUserMaps)
    {
        const OC::Path probePath = s_addonPath / path;
    
        if (OC::FileSystem::IsPathExist(probePath))
        {
            open(probePath, std::ios::in | std::ios::binary);
    
            if (is_open())
            {
                return;
            }
            else
            {
                DoHalt(OC::Format("Cannot open file %1%, permission denied or file system error.") % probePath);
            }
        }
    }
    
    if (s_isInternal)
    {
        OC::String probeName = path.filename().generic_string();
        boost::algorithm::to_upper(probeName);
    
        s_lastFilePath = probeName;
        bool found = false;
    
        OC_FOREACH(const FileTableEntry& entry, s_fileTable)
        {
            if (entry.filename() == probeName)
            {
                m_size       = entry.size();
                m_offset     = entry.offset();

                m_isEmbedded = true;

                open(BaseFile, std::ios::in | std::ios::binary);

                // TODO: error handling

                if (is_open())
                {
                    seekpos(0);
                    found = true;
                }

                break;
            }
        }

        if (!found)
        {
            DoHalt(OC::Format("Cannot find file %1% within %2%") % path % BaseFile);
        }
    }
    else
    {
        const OC::Path probePath = BaseFile / path;
    
        open(probePath, std::ios::in | std::ios::binary);
    
        if (!is_open())
        {
            DoHalt(OC::Format("Cannot open file %1%, permission denied or file system error.") % probePath);
        }
    }
}


EmbeddedFileBuffer::pos_type EmbeddedFileBuffer::seekoff(off_type off, std::ios::seekdir way, std::ios::openmode which)
{
    if (!m_isEmbedded)
    {
        return Base::seekoff(off, way, which);
    }

    if (std::ios::beg == way && off >= 0 && off <= m_size)
    {
        return Base::seekoff(m_offset + off, way, which);
    }
    else if (std::ios::cur == way)
    {
        const pos_type curPos = Base::seekoff(0, way, which) - m_offset;

        if (0 == off)
        {
            return curPos;
        }

        const pos_type newPos = curPos + off;

        if (newPos >= 0 && newPos <= m_size)
        {
            return Base::seekoff(newPos, way, which);
        }
    }
    else if (std::ios::end == way && off < 0 && off < m_size)
    {
        return Base::seekoff(m_offset + m_size + off, std::ios::beg, which);
    }

    return pos_type(off_type(-1));
}

EmbeddedFileBuffer::pos_type EmbeddedFileBuffer::seekpos(pos_type pos, std::ios::openmode which)
{
    return seekoff(off_type(pos), std::ios::beg, which);
}


// ===========================================================================


ResourceFile::ResourceFile(const OC::Path& filePath/*, const Mode mode*/)
: Base(new EmbeddedFileBuffer(filePath))
{
}

ResourceFile::~ResourceFile()
{
    delete rdbuf();
}

bool ResourceFile::is_open() const
{
    return fileBuffer()->is_open();
}

EmbeddedFileBuffer* ResourceFile::fileBuffer() const
{
    return static_cast<EmbeddedFileBuffer*>(rdbuf());
}


// ===========================================================================


namespace
{

void DumpBigFileContent(OC::File& bigFile)
{
    static bool isDumpEnabled = false;

    if (isDumpEnabled)
    {
        SDL_assert(s_isInternal);
        SDL_assert(!s_fileTable.empty());
        SDL_assert(bigFile.is_open());

        static const OC::Path DUMP_SUBDIR = OC::FileSystem::GetUserPath("dump");
        //const OC::Path DUMP_SUBDIR = "e:\\scm_bin_dump";
        OC::FileSystem::CreateDirectory(DUMP_SUBDIR);

        OC_FOREACH(const FileTableEntry& entry, s_fileTable)
        {
            bigFile.seekg(entry.offset());

            const OC::Path outPath = DUMP_SUBDIR / entry.filename();
            OC::File outFile(outPath, std::ios::out);

            for (std::streamsize bytesLeft = entry.size(); bytesLeft > 0; /* EMPTY */)
            {
                char buffer[4096];

                const std::streamsize bytesToRead = std::min(bytesLeft, std::streamsize(sizeof buffer));
                bigFile.read(buffer, bytesToRead);

                const std::streamsize bytesRead = bigFile.gcount();
                outFile.write(buffer, bytesRead);

                bytesLeft -= bytesRead;
            }
        }
    }
}

} // unnamed namespace


// ===========================================================================


void InitModule()
{
    InitVideo();

    static const OC::String DATA_FILE_NAME      = "csm.bin";
    static const OC::String DATA_DIRECTORY_NAME = "chasmdat/";

    BaseFile = OC::FileSystem::GetBasePath(DATA_FILE_NAME);

    if (OC::FileSystem::IsPathExist(BaseFile))
    {
        s_isInternal = true;
    }
    else
    {
        BaseFile = OC::FileSystem::GetUserPath(DATA_FILE_NAME);

        if (OC::FileSystem::IsPathExist(BaseFile))
        {
            s_isInternal = true;
        }
        else
        {
            BaseFile = OC::FileSystem::GetBasePath(DATA_DIRECTORY_NAME);

            if (!OC::FileSystem::IsPathExist(BaseFile))
            {
                BaseFile = OC::FileSystem::GetUserPath(DATA_DIRECTORY_NAME);

                if (!OC::FileSystem::IsPathExist(BaseFile))
                {
                    // TODO: more detailed message
                    DoHalt("Cannot find game resource file or directory.");
                }
            }
        }
    }

    if (s_isInternal)
    {
        OC::File bigFile(BaseFile);

        if (!bigFile)
        {
            DoHalt(OC::Format("Cannot open file %1%, permission denied or file system error.") % BaseFile);
        }

        Uint32 magic;
        bigFile.readBinary(magic);

        static const Sint32 CSM_ID = 0x64695343; // 'CSid'

        if (CSM_ID != magic)
        {
            DoHalt(OC::Format("Bad header in file %1%.") % BaseFile);
        }

        Uint16 fileCount;
        bigFile.readBinary(fileCount);

        for (Uint16 i = 0; i < fileCount; ++i)
        {
            FileTableEntry entry;
            entry.read(bigFile);

            s_fileTable.push_back(entry);
        }

        DumpBigFileContent(bigFile);
    }

    SDL_Log("Loading from: %s", BaseFile.string().c_str());
}


Uint16 QPifagorA32(/*...*/);
void getmousestate(/*...*/);
Sint16 GetJoyX(/*...*/);
Sint16 GetJoyY(/*...*/);
void StosW(/*...*/);
void StosEW(/*...*/);
void movsw(/*...*/);
bool ExistFile(/*...*/);
void movsD(/*...*/);
void settimer(/*...*/);
void Beep(/*...*/);
bool FExistFile(/*...*/);
void FadeOut(/*...*/);
void FadeIn(/*...*/);

void DoHalt(const char* const message)
{
    // TODO...

    if (0 == strcmp(message, "NQUIT"))
    {
        // TODO: show credits

        exit(EXIT_SUCCESS);
    }

    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Fatal Error", message, NULL);

    exit(EXIT_FAILURE);
}

void DoHalt(const OC::String& message)
{
    DoHalt(message.c_str());
}

void DoHalt(const OC::Format& message)
{
    DoHalt(message.str());
}


void ChI(const OC::BinaryStream& stream)
{
    SDL_assert(stream.good());

    if (!stream.good())
    {
        DoHalt(OC::Format("Error while reading %1%, permission denied or file system error.") % s_lastFilePath);
    }
}

//void GetMem16(/*...*/);
//void FreeMem16(/*...*/);
void CalcDir(/*...*/);
Sint16 Max(/*...*/);
Sint16 Min(/*...*/);
Sint8 Sgn(/*...*/);
void SetCurPicTo(/*...*/);
void LoadPic(/*...*/);
void LoadAnimation(/*...*/);
void LoadPOH(/*...*/);
void ScanLoHi(/*...*/);
void ScanLowHigh(/*...*/);
void InitCaracter(/*...*/);
void UpLoadCaracter(/*...*/);
void ReleaseCaracter(/*...*/);
void LoadSound(/*...*/);
void LoadAmb(/*...*/);
void AllocVideo(/*...*/);
void AllocMemory(/*...*/);
void LoadCommonParts(/*...*/);
void CheckMouse(/*...*/);
void RemoveEqual(/*...*/);
void ScanLevels(/*...*/);
void FindNextLevel(/*...*/);
void LoadGraphics(/*...*/);
void LoadGround(/*...*/);
void DoSetPalette(/*...*/);
void SetPalette(/*...*/);
void AddEvent(/*...*/);
void AddEvVoice(/*...*/);
void Hline(/*...*/);
void Vline(/*...*/);
void HBrline(/*...*/);
void VBrline(/*...*/);
void DrawKey(/*...*/);
void DrawBrKey(/*...*/);
void BrightBar(/*...*/);
void ShowMap(/*...*/);
Uint16 CalcStringLen(/*...*/);
void ReInitViewConst(/*...*/);
void AddBlowLight(/*...*/);
void _AddBlowLight(/*...*/);
void AddBlow(/*...*/);
void BlowSpark(/*...*/);
void BlowSmoke(/*...*/);
void BlowShootSmoke(/*...*/);
void PutConsMessage(/*...*/);
void PutConsMessage2(/*...*/);
void PutConsMessage3(/*...*/);

Uint16 ServerVersion;
Sint32 Long1;
Uint8 LB;
OC::String::value_type ConsoleCommands[546];
OC::String::value_type ncNames[24];
Sint16 ncSDivs[5];
Sint16 ncYDelta[3];
VideoPos__Element VideoPos[4];
Uint8 ReColorShift[8];
OC::String::value_type OnOff[10];
Sint16 Fr;
Sint16 Fr2;
Sint16 Frr;
void* ShadowMap;
void* WShadowMap;
void* ShadowMap2;
void* WShadowMap2;
Uint16 FlSegs[256];
TPic Pc;
TPic CurPic;
Uint16 CurPicSeg;
Uint16 ShadowSeg;
Uint16 WShadowSeg;
Uint16 ShadowSeg2;
Uint16 WShadowSeg2;
Uint16 cwc;
Uint16 CurShOfs;
Uint16 CMP0;
Uint16 XORMask;

OC::String::value_type* GFXindex;
OC::String::value_type* ShortNames;
OC::String::value_type* LevelNames;
Uint8 ColorMap[13312];
Uint8 FloorMap[4096];
Uint8 CellMap[4096];
Uint8* AltXTab;
TLight* Lights;
TTPort__Element* Tports;
void* PImPtr[120];
Uint16 PImSeg[120];
Uint8 WallMask[120];
TObjBMPInfo ObjBMPInf[4];
TObj3DInfo Obj3DInf[96];
Uint16 LinesH1[847];
Uint16 LinesH2[847];
TLinesBuf* LinesBUF;
THoleItem* HolesList;
Uint8 SpryteUsed[120];
Uint16 Mul320[201];
Sint32 MulSW[701];
Sint16 SinTab[1024];
TLoc* Map;
OC::String::value_type* ConsHistory;
Uint8* VMask;
Uint8* Flags;
Uint8* DarkMap;
Uint8* AmbMap;
Uint8* TeleMap;
Uint8* FloorZHi;
Uint8* FloorZLo;
EndCamera__Type EndCamera;
TEvent EventsList[16];
TFrame* FramesList;
TBlow* BlowsList;
TMonster MonstersList[90];
TRocket RocketList[64];
TSepPart SepList[32];
TMine MinesList[16];
TBlowLight BlowLights[32];
TMonsterInfo MonstersInfo[23];
TRocketInfo RocketsInfo[32];
TSepPartInfo SepPartInfo[90];
TBlowInfo BlowsInfo[24];
TReObject* ReObjects;
TAmmoBag* AmmoBags;
bool FFlags[64];
Uint8* Fonts;
Uint8* BigFont;
Uint8* LitFont;
Uint8* WIcons;
Sint8 Palette[768];
Sint8 Pal[768];
Uint16 CharSize[256];
TGunInfo GunsInfo[9];
NetPlace__Element NetPlace[32];
void* VGA;
void* Ground;
void* Status;
void* Loading;
void* VesaTiler;
void* SkyPtr;
void* RGBTab25;
void* RGBTab60;
Uint16 LoadPos;
Uint16 LoadingH;
Uint16 LoadingW;
OC::Real ca;
OC::Real sa;
Sint16 RShadeDir;
Sint16 RShadeLev;
Sint16 LastRShadeLev;
Sint16 BShadeDir;
Sint16 BShadeLev;
Sint16 LastBShadeLev;
Sint16 GShadeDir;
Sint16 GShadeLev;
Sint16 LastGShadeLev;
Uint16 MenuCode;
Uint16 CSCopy;
Uint16 LoadSaveY;
Uint16 OptionsY;
Uint8 NetMode;
Uint8 TeamMode;
Uint8 SHRC;
TMonster Ms;
Sint16 Skill;
Sint16 MyNetN;
Sint16 bpx;
Sint16 bpy;
void* ConsolePtr;
OC::String::value_type ConsoleComm[287];
Sint16 ConsY;
Sint16 ConsDY;
Sint16 ConsMode;
Sint16 ConsMainY;
Sint16 ConsMenu;
Sint16 HistCnt;
Sint16 CurHist;
Sint16 MenuMode;
Sint16 MenuMainY;
Uint8 MSsens;
Sint16 DisplaySett[3];
Sint16 Contrast;
Sint16 Color;
Sint16 Bright;
Sint16 LandZ;
Sint16 CeilZ;
Sint16 EndDelay;
Uint16 Hz;
Uint16 Hz2;
Uint16 DMax;
Uint16 Dy;
Uint16 DDY;
Uint16 oldy;
Uint16 _x1;
Uint16 _x2;
Uint16 _X;
Uint16 sl;
Uint16 slp;
Uint16 t1;
Uint16 Times10Sum;
Uint16 _Times10Sum;
Uint16 Frames10Count;
Uint16 CurTime;
Uint16 LastGunNumber;
Uint16 GunShift;
Uint16 RespawnTime;
Uint16 ys31;
Uint16 ys32;
Uint16 ys11;
Uint16 ys12;
Uint16 ys21;
Uint16 ys22;
Uint16 sh1;
Uint16 sh2;
Uint16 mysy;
Uint16 ObjectsLoaded;
Uint16 WeaponsCount;
Uint16 MCount;
Uint16 TotalKills;
Uint16 TotalKeys;
Uint16 RCount;
Uint16 BCount;
Uint16 DCount;
Uint16 FCount;
Uint16 LCount;
Uint16 LtCount;
Uint16 SCount;
Uint16 TCount;
Uint16 MnCount;
Uint16 AmCount;
Uint16 ReCount;
Uint16 HolCount;
Uint16 SFXSCount;
Uint16 InfoLen;
Uint16 Di0;
Uint16 Dx0;
Uint16 Recsize;
Uint16 MTime;
Uint16 WeaponFTime;
Uint16 WeaponPhase;
Uint16 j;
Uint16 w;
Uint16 BLevelDef;
Uint16 BLevel0;
Uint16 BLevelW;
Uint16 BlevelC;
Uint16 BLevelF;
Uint16 VgaSeg;
Uint16 RGBSeg;
Uint16 RGBSeg25;
Uint16 RGBSeg60;
Uint16 PSeg;
Uint16 POfs;
Uint16 PImBSeg;
Uint16 ImSeg;
Uint16 ImOfs;
Uint16 Ims;
Uint16 ImSegS;
Uint16 MyDeath;
Uint16 ObjectX;
Uint16 ObjectY;
Uint16 ObjSeg;
Uint16 HlBr;
Uint16 HLxx;
Uint16 HLh1;
Uint16 Hlh2;
Uint16 Hlhr1;
Uint16 Hlhr2;
Uint16 HLRH;
Uint16 HLFh;
Uint16 YMin1;
Uint16 YMin2;
Uint16 Nlines;
Uint16 NLines1;
Uint16 takt;
Uint16 MsTakt;
Uint16 cm_ofs;
Uint16 b0;
Uint16 b1;
bool WeaponActive;
Sint32 CellV;
Sint32 FloorV;
Sint32 HLSt;
Sint16 WXSize;
Sint16 isa;
Sint16 ica;
Sint16 ica2;
Sint16 isa2;
Sint16 isa4;
Sint16 ica4;
Sint16 SxS;
Sint16 Sx11;
Sint16 Sx21;
Sint16 Sx1;
Sint16 Sx2;
Sint16 DShift;
Sint16 Dir;
Sint16 WpnShx;
Sint16 WpnShy;
Sint16 lvz;
Sint16 hvz;
Sint16 GunDx;
Sint16 GunDy;
Sint16 ShakeLevel;
Sint16 LookVz;
Sint16 _LookVz;
Sint16 v1x;
Sint16 v1y;
Sint16 v2x;
Sint16 v2y;
Sint16 nx;
Sint16 ny;
Sint16 HS;
Sint16 Hx;
Sint16 Hy;
Sint16 HMx;
Sint16 HMy;
Sint16 ehx;
Sint16 ehy;
Sint16 ehz;
Sint16 x;
Sint16 y;
Sint16 mi;
Sint16 MapR;
Sint16 LevelN;
Sint16 ox;
Sint16 HRv;
Uint16 HksFlags;
Uint16 HFi;
Uint16 EHFi;
bool FirstTakt;
bool InfoPage;
bool NextL;
bool SERVER;
bool CLIENT;
bool Paused;
bool Monsters;
bool Animation;
bool TimeInd;
bool FullMap;
bool EndOfTheGame;
bool ClipMode;
bool IamDead;
bool Slow;
bool Spline;
bool Ranking;
bool NewSecond;
bool OAnimate;
bool MapMode;
bool ExMode;
bool Chojin;
bool TabMode;
bool SafeLoad;
bool EpisodeReset;
bool AlwaysRun;
bool ReverseMouse;
bool SkyVisible;
bool MenuOn;
bool Console;
bool Cocpit;
bool GameActive;
Uint8 SecCounter;
void (*TimerInt)();
void (*KbdInt)();
Sint32 EDI0;
Sint32 Edi1;
Sint32 mEDX;
Sint32 Ll;
Sint32 Time0;
Sint32 Time;
Sint32 ZTime;
Sint32 mem0;
Sint32 k;
Sint32 RealTime;
Sint32 LastMouseTime;
Sint32 LastPainTime;
Sint32 StartUpRandSeed;
Sint32 MSRND;
TLoc L;
Sint32 VPSize;
Sint32 VideoH;
Sint32 VideoW;
Sint32 VideoBPL;
Uint16 VideoEX;
Uint16 VideoEY;
Uint16 VideoCX;
Uint16 VideoCY;
bool VideoIsFlat;
Uint16 WinW;
Uint16 WinW2;
Uint16 WinSX;
Uint16 WinEX;
Uint16 WinSY;
Uint16 WinEY;
Uint16 WinH;
Uint16 WinH2u;
Uint16 WinH2d;
Uint16 WinCY;
Uint16 WallH;
Uint16 WallW;
Uint16 ObjectW;
Uint16 WinB;
Uint16 WinB3;
Uint16 WinE;
Uint16 WinE3;
Sint16 WMapX1;
Sint16 WMapX2;
Sint16 WMapY1;
Sint16 WMapY2;
Sint16 WMapX;
Sint16 WMapY;
Sint16 mpk_x;
Sint16 mpk_y;
Sint16 mps;
Sint16 WinW2i;
Sint16 StepMove;
Sint16 FLeftEnd;
Sint16 FRightEnd;
Sint16 CLeftEnd;
Sint16 CRightEnd;
Sint16 FLE160;
Sint16 DNpy;
Sint16 DNpx;
Sint16 FMapDx;
Sint16 FMapDy;
Sint16 FShwDx;
Sint16 FShwDy;
Uint16 HXiFF;
Uint16 FloorW;
Uint16 FloorDiv;
Uint16 TMy;
Uint16 DNp;
Uint16 FCurMapOfs;
Uint16 FMx;
Uint16 FMy;
Sint32 Esl;
Sint32 Io0;
Sint32 SlY2;
Sint32 SLy;
Sint16 tx1;
Sint16 tx2;
Sint16 ty1;
Sint16 ty2;
Sint16 _ty1;
Sint16 DDark;
Uint16 WShadowOfs;
Uint16 CMPOfs;
Uint16 XOfsMask;
Uint16 Cnt;
Uint16 FromOfs;
Uint16 FullH;
Uint16 yf0;
Uint16 yf1;
Uint16 yf2;
Uint16 ScrollK;
Uint8 Double;
Sint16 RSize;
Sint16 rrx;
Sint16 rry;
Sint16 nhxi;
Sint16 nhyi;
Sint16 aLwx;
Sint16 aLWy;
Sint16 DSh;
Sint16 LWx;
Sint16 LWy;
Sint16 Rx;
Sint16 Ry;
Sint16 R;
bool KeysState[128];
Uint8 KBDBuf[16];
Uint16 KBDBufCnt;
Uint8 KeysID[16];
Uint8 _FrontOn;
Uint8 _BackOn;
Uint8 _LeftOn;
Uint8 _RightOn;
Uint8 _SLeftOn;
Uint8 _SRightOn;
Uint8 _JumpOn;
Uint8 _FireOn;
Uint8 _ChangOn;
Uint8 _StrafeOn;
Uint8 _SpeedUpOn;
Uint8 _MLookOn;
Uint8 _MLookT;
Uint8 _ViewUpOn;
Uint8 _ViewCntrOn;
Uint8 _ViewDnOn;
Uint8 Ms1ID;
Uint8 Ms2ID;
Uint8 ms3id;
bool kbViewUp;
bool kbViewDn;
bool kbViewCntr;
bool VCenterMode;
Uint8 c;
Uint8 kod;
Uint8 key;
Uint8 ASCII_Tab[256];
OC::String::value_type S[256];
//Dos::Registers Regs;
Sint16 JoyX;
Sint16 JoyY;
Sint16 JoyCrX;
Sint16 JoyCrY;
Sint16 JoyMnX;
Sint16 JoyMnY;
Sint16 JoyMxX;
Sint16 JoyMxY;
bool JoyStick;
bool JoyKeyA;
bool JoyKeyB;
Sint16 MsX;
Sint16 MsY;
Sint16 MsButt;
Sint16 MsV;
Sint16 MLookTime;
Sint16 LastMouseX;
Sint16 LastMouseY;
Sint16 MsRv;
Sint16 MsVV;
Sint16 Msvvi;
Sint16 msrvi;
bool MouseD;
bool MsKeyA;
bool MsKeyB;
bool MsKeyC;
bool MLookOn;
bool RecordDemo;
bool PlayDemo;
bool IPXPresent;
Uint8 NGMode;
Uint8 NGTeam;
Uint8 NGSkill;
Uint8 NGLevel;
Uint8 NGCard;
Uint8 NGPort;
Uint8 NGBaud;
Uint8 NGColor;
OC::String::value_type NGNick[9];
OC::String::value_type SelfNick[9];
Uint8 SelfColor;
TInfo_Struct* PInfoStruct;
bool MSCDEX;
Sint16 LCDTrack;
Sint16 CDTrack;
Sint32 CDTime;
TPlayerInfo Players[8];
Uint8 LastBorn;
Uint8 LevelChanges[16];
Uint16 ProcState[4];
OC::String::value_type NetMessage[33];
MessageRec__Element MessageRec[4];
ProcMessage__Type ProcMessage;
Uint8 VESAError;
Uint16 VESABank;
Uint16 VESABankShift;
Uint8 VESA_CurrColor;
bool VESAPresent;
bool VESA20Compliant;
Uint16 VESAVersion;
Uint16 TotalMemory;
Uint16 FLATSelector;
Sint32 VESA20Addr;
ModesList__Type ModesList;
TDPMIRegs DRegs;
bool NETMonitor;
bool InBrifing;
Uint8 VideoOwners[4];
Uint16 CurOwner;
Uint16 CurVideoMode = 1;
Uint16 LastVideoMode;
ServerSaved__Type ServerSaved;
Sint16 CurWindow;

bool TextSeek(/*...*/);
void ShowFinalScreen(/*...*/);
void Wait_R(/*...*/);
void LoadPicsPacket(/*...*/);
void ScanWH(/*...*/);
void AllocFloors(/*...*/);
void LoadSounds(/*...*/);
void LoadBMPObjects(/*...*/);
void Load3dObjects(/*...*/);
void LoadRockets(/*...*/);
void LoadGibs(/*...*/);
void LoadBlows(/*...*/);
void LoadMonsters(/*...*/);
void LoadGunsInfo(/*...*/);
void CLine(/*...*/);
void Line(/*...*/);
void HBrline0(/*...*/);
void InitNormalViewHi(/*...*/);
void InitMonitorView(/*...*/);

OC::Path BaseFile;

} // namespace CSPBIO
