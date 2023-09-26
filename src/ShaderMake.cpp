/*
Copyright (c) 2014-2023, NVIDIA CORPORATION. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "argparse.h"
#include <ShaderMake/ShaderBlob.h>

#include <sstream>
#include <fstream>
#include <map>
#include <vector>
#include <list>
#include <regex>
#include <thread>
#include <mutex>
#include <filesystem>
#include <atomic>
#include <cstdio>
#include <csignal>
#include <cstdarg>
#include <cstring>

#ifdef _WIN32
    #include <d3dcompiler.h> // FXC
    #include <dxcapi.h> // DXC

    #include <wrl/client.h>
    using Microsoft::WRL::ComPtr;
#else
    #include <unistd.h>
    #include <limits.h>
#endif

using namespace std;
namespace fs = filesystem;

#define _L(x)  __L(x)
#define __L(x) L ## x
#define UNUSED(x) ((void)(x))
#define COUNT_OF(a) (sizeof(a) / sizeof(a[0]))

#define USE_GLOBAL_OPTIMIZATION_LEVEL 0xFF
#define SPIRV_SPACES_NUM 8
#define PDB_DIR "PDB"

#ifdef _MSC_VER
    #define popen _popen
    #define pclose _pclose
    #define putenv _putenv
#endif

enum Platform : uint8_t
{
    DXBC,
    DXIL,
    SPIRV,

    PLATFORMS_NUM
};

struct Options
{
    vector<fs::path> includeDirs;
    vector<fs::path> relaxedIncludes;
    vector<string> defines;
    vector<string> spirvExtensions = {"SPV_EXT_descriptor_indexing", "KHR"};
    fs::path configFile;
    const char* platformName = nullptr;
    const char* outputDir = nullptr;
    const char* shaderModel = "6_5";
    const char* vulkanVersion = "1.3";
    const char* sourceDir = "";
    const char* compiler = nullptr;
    const char* outputExt = nullptr;
    uint32_t sRegShift = 100; // must be first (or change "DxcCompile" code)
    uint32_t tRegShift = 200;
    uint32_t bRegShift = 300;
    uint32_t uRegShift = 400;
    uint32_t optimizationLevel = 3;
    Platform platform = DXBC;
    bool serial = false;
    bool flatten = false;
    bool force = false;
    bool help = false;
    bool binary = false;
    bool header = false;
    bool binaryBlob = false;
    bool headerBlob = false;
    bool continueOnError = false;
    bool warningsAreErrors = false;
    bool allResourcesBound = false;
    bool pdb = false;
    bool stripReflection = false;
    bool matrixRowMajor = false;
    bool hlsl2021 = false;
    bool verbose = false;
    bool colorize = false;
    bool useAPI = false;

    bool Parse(int32_t argc, const char** argv);

    inline bool IsBlob() const
    { return binaryBlob || headerBlob; }
};

struct ConfigLine
{
    vector<string> defines;
    const char* source = nullptr;
    const char* entryPoint = "main";
    const char* profile = nullptr;
    const char* outputDir = nullptr;
    uint32_t optimizationLevel = USE_GLOBAL_OPTIMIZATION_LEVEL;

    bool Parse(int32_t argc, const char** argv);
};

struct TaskData
{
    vector<string> defines;
    string source;
    string entryPoint;
    string profile;
    string outputFileWithoutExt;
    string combinedDefines;
    uint32_t optimizationLevel = 3;
};

struct BlobEntry
{
    string permutationFileWithoutExt;
    string combinedDefines;
};

Options g_Options;
map<fs::path, fs::file_time_type> g_HierarchicalUpdateTimes;
map<string, vector<BlobEntry>> g_ShaderBlobs;
vector<TaskData> g_TaskData;
mutex g_TaskMutex;
atomic<uint32_t> g_ProcessedTaskCount;
atomic<bool> g_Terminate = false;
atomic<uint32_t> g_FailedTaskCount = 0;
uint32_t g_OriginalTaskCount;
const char* g_OutputExt = nullptr;

static const char* g_PlatformNames[] = {
    "DXBC",
    "DXIL",
    "SPIRV",
};

static const char* g_PlatformExts[] = {
    ".dxbc",
    ".dxil",
    ".spirv",
};

#if 1
    #define RED "\x1b[31m"
    #define GRAY "\x1b[90m"
    #define WHITE "\x1b[0m"
    #define GREEN "\x1b[32m"
    #define YELLOW "\x1b[33m"
#else
    #define RED ""
    #define GRAY ""
    #define WHITE ""
    #define GREEN ""
    #define YELLOW ""
#endif

/*
Naming convention:
    - file        - "path/to/name{.ext}"
    - name        - "name" from "file"
    - path        - "path" from "file"
    - permutation - "name" + "hash"
    - stream      - FILE or ifstream
*/

//=====================================================================================================================
// MISC
//=====================================================================================================================

inline uint32_t HashToUint(size_t hash)
{ return uint32_t(hash) ^ (uint32_t(hash >> 32)); }

inline string PathToString(fs::path path)
{ return path.lexically_normal().make_preferred().string(); }

inline wstring AnsiToWide(const string& s)
{ return wstring(s.begin(), s.end()); }

inline fs::path RemoveLeadingDotDots(const fs::path& path)
{
    auto it = path.begin();
    while (*it == ".." && it != path.end())
        ++it;

    fs::path result;
    while (it != path.end())
    {
        result = result / *it;
        ++it;
    }

    return result;
}

inline bool IsSpace(char ch)
{ return strchr(" \t\r\n", ch) != nullptr; }

inline bool HasRepeatingSpace(char a, char b)
{ return (a == b) && a == ' '; }

inline string EscapePath(const string& s)
{
    if (s.find(' ') != string::npos)
        return "\"" + s + "\"";

    return s;
}

inline void TrimConfigLine(string& s)
{
    // Remove leading whitespace
    s.erase(s.begin(), find_if(s.begin(), s.end(), [](char ch) { return !IsSpace(ch); }));

    // Remove trailing whitespace
    s.erase(find_if(s.rbegin(), s.rend(), [](char ch) { return !IsSpace(ch); }).base(), s.end());

    // Tabs to spaces
    replace(s.begin(), s.end(), '\t', ' ');

    // Remove double spaces
    string::iterator newEnd = unique(s.begin(), s.end(), HasRepeatingSpace);
    s.erase(newEnd, s.end());
}

void TokenizeConfigLine(char* in, vector<const char*>& tokens)
{
    char* out = in;
    char* token = out;

    // Some magic to correctly tokenize spaces in ""
    bool isString = false;
    while (*in)
    {
        if (*in == '"')
            isString = !isString;
        else if (*in == ' ' && !isString)
        {
            *in = '\0';
            if (*token)
                tokens.push_back(token);
            token = out + 1;
        }

        if (*in != '"')
            *out++ = *in;

        in++;
    }
    *out = '\0';

    if (*token)
        tokens.push_back(token);
}

uint32_t GetFileLength(FILE* stream)
{
    /*
    TODO: can be done more efficiently
    Win:
        #include <io.h>

        _filelength( _fileno(f) );
    Linux:
        #include <sys/types.h> //?
        #include <sys/stat.h>

        struct stat buf;
        fstat(fd, &buf);
        off_t size = buf.st_size;
    */

    const uint32_t pos = ftell(stream);
    fseek(stream, 0, SEEK_END);
    const uint32_t len = ftell(stream);
    fseek(stream, pos, SEEK_SET);

    return len;
}

void Printf(const char* format, ...)
{
    va_list argptr;
    va_start(argptr, format);

    char fixedFormat[512];
    if (!g_Options.colorize)
    {
        const char* in = format;
        char* out = fixedFormat;

        while (*in)
        {
            if (*in == '\x1b')
                while( *in++ != 'm' );

            *out++ = *in++;
        }
        *out = '\0';

        format = fixedFormat;
    }

    vprintf(format, argptr);
    va_end(argptr);

    // IMPORTANT: needed only if being run in CMake environment
    fflush(stdout);
}

string GetShaderName(const fs::path& path)
{
    string name = path.filename().string();
    replace(name.begin(), name.end(), '.', '_');
    name += "_" + string(g_PlatformExts[g_Options.platform] + 1);

    return "g_" + name;
}

// A class that is used to write a code blob as binary or C-string
class DataOutputContext
{
public:
    FILE* stream = nullptr;

    DataOutputContext(const char* file, bool textMode)
    {
        stream = fopen(file, textMode ? "w": "wb");
        if (!stream)
            Printf(RED "ERROR: Can't open file '%s' for writing!\n", file);
    }

    ~DataOutputContext()
    {
        if (stream)
        {
            fclose(stream);
            stream = nullptr;
        }
    }

    bool WriteDataAsText(const void* data, size_t size)
    {
        for (size_t i = 0; i < size; i++)
        {
            uint8_t value = ((const uint8_t*)data)[i];

            if (m_lineLength > 128)
            {
                fprintf(stream, "\n    ");
                m_lineLength = 0;
            }

            fprintf(stream, "%u, ", value);

            if (value < 10)
                m_lineLength += 3;
            else if (value < 100)
                m_lineLength += 4;
            else
                m_lineLength += 5;
        }

        return true;
    }

    void WriteTextPreamble(const char* shaderName)
    { fprintf(stream, "const uint8_t %s[] = {", shaderName); }

    void WriteTextEpilog()
    { fprintf(stream, "\n};\n"); }

    bool WriteDataAsBinary(const void* data, size_t size)
    {
        if (size == 0)
            return true;

        return fwrite(data, size, 1, stream) == 1;
    }

    // For use as a callback in "WriteFileHeader" and "WritePermutation" functions
    static bool WriteDataAsTextCallback(const void* data, size_t size, void* context)
    { return ((DataOutputContext*)context)->WriteDataAsText(data, size); }

    static bool WriteDataAsBinaryCallback(const void* data, size_t size, void* context)
    { return ((DataOutputContext*)context)->WriteDataAsBinary(data, size); }

private:
    uint32_t m_lineLength = 129;
};

void DumpShader(const TaskData& taskData, const uint8_t* data, size_t dataSize)
{
    string file = taskData.outputFileWithoutExt + g_OutputExt;

    if (g_Options.binary || g_Options.IsBlob())
    {
        DataOutputContext context(file.c_str(), false);
        if (!context.stream)
            return;

        context.WriteDataAsBinary(data, dataSize);
    }

    if (g_Options.header)
    {
        DataOutputContext context((file + ".h").c_str(), true);
        if (!context.stream)
            return;

        string shaderName = GetShaderName(taskData.outputFileWithoutExt);
        context.WriteTextPreamble(shaderName.c_str());
        context.WriteDataAsText(data, dataSize);
        context.WriteTextEpilog();
    }
}

void UpdateProgress(const TaskData& taskData, bool isSucceeded, const char* message)
{
    // IMPORTANT: do not split into several "Printf" calls because multi-threading access to the console can mess up the order
    if (isSucceeded)
    {
        float progress = 100.0f * float(++g_ProcessedTaskCount) / float(g_OriginalTaskCount);

        // DXC from Win SDK is always outdated. DXC from VK SDK doesn't sign. Ignore warning about signing to avoid spam...
        if (message && !strstr(message, "DXIL signing library"))
        {
            Printf(YELLOW "[%5.1f%%] %s %s {%s} {%s}\n%s",
                progress, g_Options.platformName,
                taskData.source.c_str(),
                taskData.entryPoint.c_str(),
                taskData.combinedDefines.c_str(),
                message);
        }
        else
        {
            Printf(GREEN "[%5.1f%%]" GRAY " %s" WHITE " %s" GRAY " {%s}" WHITE " {%s}\n",
                progress, g_Options.platformName,
                taskData.source.c_str(),
                taskData.entryPoint.c_str(),
                taskData.combinedDefines.c_str());
        }
    }
    else
    {
        Printf(RED "[ FAIL ] %s %s {%s} {%s}\n%s",
            g_Options.platformName,
            taskData.source.c_str(),
            taskData.entryPoint.c_str(),
            taskData.combinedDefines.c_str(),
            message ? message : "<no message text>!\n");

        if (!g_Options.continueOnError)
            g_Terminate = true;

        ++g_FailedTaskCount;
    }
}

//=====================================================================================================================
// TIMER
//=====================================================================================================================

double g_TicksToMilliseconds;

double Timer_ConvertTicksToMilliseconds(uint64_t ticks)
{ return (double)ticks * g_TicksToMilliseconds; }

void Timer_Init()
{
#ifdef _WIN32
    uint64_t ticksPerSecond = 1;
    QueryPerformanceFrequency((LARGE_INTEGER*)&ticksPerSecond);

    g_TicksToMilliseconds = 1000.0 / ticksPerSecond;
#else
    g_TicksToMilliseconds = 1.0 / 1000000.0;
#endif
}

uint64_t Timer_GetTicks()
{
#ifdef _WIN32
    uint64_t ticks;
    QueryPerformanceCounter((LARGE_INTEGER*)&ticks);

    return ticks;
#else
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);

    return uint64_t(spec.tv_sec) * 1000000000ull + spec.tv_nsec;
#endif
}

//=====================================================================================================================
// OPTIONS
//=====================================================================================================================

int AddInclude(struct argparse* self, const struct argparse_option* option)
{ ((Options*)(option->data))->includeDirs.push_back(*(const char**)option->value); UNUSED(self); return 0; }

int AddGlobalDefine(struct argparse* self, const struct argparse_option* option)
{ ((Options*)(option->data))->defines.push_back(*(const char**)option->value); UNUSED(self); return 0; }

int AddRelaxedInclude(struct argparse* self, const struct argparse_option* option)
{ ((Options*)(option->data))->relaxedIncludes.push_back(*(const char**)option->value); UNUSED(self); return 0; }

int AddSpirvExtension(struct argparse* self, const struct argparse_option* option)
{ ((Options*)(option->data))->spirvExtensions.push_back(*(const char**)option->value); UNUSED(self); return 0; }

bool Options::Parse(int32_t argc, const char** argv)
{
    const char* config = nullptr;
    const char* unused = nullptr; // storage for callbacks

    struct argparse_option options[] = {
        OPT_HELP(),
        OPT_GROUP("Required options:"),
            OPT_STRING('p', "platform", &platformName, "DXBC, DXIL or SPIRV", nullptr, 0, 0),
            OPT_STRING('c', "config", &config, "Configuration file with the list of shaders to compile", nullptr, 0, 0),
            OPT_STRING('o', "out", &outputDir, "Output directory", nullptr, 0, 0),
            OPT_BOOLEAN('b', "binary", &binary, "Output binary files", nullptr, 0, 0),
            OPT_BOOLEAN('h', "header", &header, "Output header files", nullptr, 0, 0),
            OPT_BOOLEAN('B', "binaryBlob", &binaryBlob, "Output binary blob files", nullptr, 0, 0),
            OPT_BOOLEAN('H', "headerBlob", &headerBlob, "Output header blob files", nullptr, 0, 0),
            OPT_STRING(0, "compiler", &compiler, "Path to a FXC/DXC compiler", nullptr, 0, 0),
        OPT_GROUP("Compiler settings:"),
            OPT_STRING('m', "shaderModel", &shaderModel, "Shader model for DXIL/SPIRV (always SM 5.0 for DXBC)", nullptr, 0, 0),
            OPT_INTEGER('O', "optimization", &optimizationLevel, "Optimization level 0-3 (default = 3, disabled = 0)", nullptr, 0, 0),
            OPT_BOOLEAN(0, "WX", &warningsAreErrors, "Maps to '-WX' DXC/FXC option: warnings are errors", nullptr, 0, 0),
            OPT_BOOLEAN(0, "allResourcesBound", &allResourcesBound, "Maps to '-all_resources_bound' DXC/FXC option: all resources bound", nullptr, 0, 0),
            OPT_BOOLEAN(0, "PDB", &pdb, "Output PDB files in 'out/PDB/' folder", nullptr, 0, 0),
            OPT_BOOLEAN(0, "stripReflection", &stripReflection, "Maps to '-Qstrip_reflect' DXC/FXC option: strip reflection information from a shader binary", nullptr, 0, 0),
            OPT_BOOLEAN(0, "matrixRowMajor", &matrixRowMajor, "Maps to '-Zpr' DXC/FXC option: pack matrices in row-major order", nullptr, 0, 0),
            OPT_BOOLEAN(0, "hlsl2021", &hlsl2021, "Maps to '-HV 2021' DXC option: enable HLSL 2021 standard", nullptr, 0, 0),
        OPT_GROUP("Defines & include directories:"),
            OPT_STRING('I', "include", &unused, "Include directory(s)", AddInclude, (intptr_t)this, 0),
            OPT_STRING('D', "define", &unused, "Macro definition(s) in forms 'M=value' or 'M'", AddGlobalDefine, (intptr_t)this, 0),
        OPT_GROUP("Other options:"),
            OPT_BOOLEAN('f', "force", &force, "Treat all source files as modified", nullptr, 0, 0),
            OPT_STRING(0, "sourceDir", &sourceDir, "Source code directory", nullptr, 0, 0),
            OPT_STRING(0, "relaxedInclude", &unused, "Include file(s) not invoking re-compilation", AddRelaxedInclude, (intptr_t)this, 0),
            OPT_STRING(0, "outputExt", &outputExt, "Extension for output files, default is one of .dxbc, .dxil, .spirv", AddRelaxedInclude, (intptr_t)this, 0),
            OPT_BOOLEAN(0, "serial", &serial, "Disable multi-threading", nullptr, 0, 0),
            OPT_BOOLEAN(0, "flatten", &flatten, "Flatten source directory structure in the output directory", nullptr, 0, 0),
            OPT_BOOLEAN(0, "continue", &continueOnError, "Continue compilation if an error is occured", nullptr, 0, 0),
#ifdef _WIN32
            OPT_BOOLEAN(0, "useAPI", &useAPI, "Use FXC (d3dcompiler) or DXC (dxcompiler) API explicitly (Windows only)", nullptr, 0, 0),
#endif
            OPT_BOOLEAN(0, "colorize", &colorize, "Colorize console output", nullptr, 0, 0),
            OPT_BOOLEAN(0, "verbose", &verbose, "Print commands before they are executed", nullptr, 0, 0),
        OPT_GROUP("SPIRV options:"),
            OPT_STRING(0, "vulkanVersion", &vulkanVersion, "Vulkan environment version, maps to '-fspv-target-env' (default = 1.3)", nullptr, 0, 0),
            OPT_STRING(0, "spirvExt", &unused, "Maps to '-fspv-extension' option: add SPIR-V extension permitted to use", AddSpirvExtension, (intptr_t)this, 0),
            OPT_INTEGER(0, "sRegShift", &sRegShift, "SPIRV: register shift for sampler (s#) resources", nullptr, 0, 0),
            OPT_INTEGER(0, "tRegShift", &tRegShift, "SPIRV: register shift for texture (t#) resources", nullptr, 0, 0),
            OPT_INTEGER(0, "bRegShift", &bRegShift, "SPIRV: register shift for constant (b#) resources", nullptr, 0, 0),
            OPT_INTEGER(0, "uRegShift", &uRegShift, "SPIRV: register shift for UAV (u#) resources", nullptr, 0, 0),
        OPT_END(),
    };

    static const char* usages[] = {
        "ShaderMake.exe -p {DXBC|DXIL|SPIRV} --binary [--header --blob] -c \"path/to/config\"\n"
        "\t-o \"path/to/output\" --compiler \"path/to/compiler\" [other options]\n"
        "\t-D DEF1 -D DEF2=1 ... -I \"path1\" -I \"path2\" ...",
        nullptr
    };

    struct argparse argparse;
    argparse_init(&argparse, options, usages, 0);
    argparse_describe(&argparse, nullptr, "\nMulti-threaded shader compiling & processing tool");
    argparse_parse(&argparse, argc, argv);

    if (!config)
    {
        Printf(RED "ERROR: Config file not specified!\n");
        return false;
    }

    if (!fs::exists(config))
    {
        Printf(RED "ERROR: Config file '%s' does not exist!\n", config);
        return false;
    }

    if (!outputDir)
    {
        Printf(RED "ERROR: Output directory not specified!\n");
        return false;
    }

    if (!binary && !header && !binaryBlob && !headerBlob)
    {
        Printf(RED "ERROR: One of 'binary', 'header', 'binaryBlob' or 'headerBlob' must be set!\n");
        return false;
    }
    if (!platformName)
    {
        Printf(RED "ERROR: Platform not specified!\n");
        return false;
    }

    if (!compiler)
    {
        Printf(RED "ERROR: Compiler not specified!\n");
        return false;
    }

    if (!fs::exists(compiler))
    {
        Printf(RED "ERROR: Compiler '%s' does not exist!\n", compiler);
        return false;
    }

    if (strlen(shaderModel) != 3 || strstr(shaderModel, "."))
    {
        Printf(RED "ERROR: Shader model ('%s') must have format 'X_Y'!\n", shaderModel);
        return false;
    }

    // Platform
    uint32_t i = 0;
    for (; i < PLATFORMS_NUM; i++)
    {
        if (!strcmp(platformName, g_PlatformNames[i]))
        {
            platform = (Platform)i;
            break;
        }
    }
    if (i == PLATFORMS_NUM)
    {
        Printf(RED "ERROR: Unrecognized platform '%s'!\n", platformName);
        return false;
    }

    if (outputExt)
        g_OutputExt = outputExt;
    else
        g_OutputExt = g_PlatformExts[platform];

    // Absolute path is needed for source files to get "clickable" messages
#ifdef _WIN32
    char cd[MAX_PATH];
    if (!GetCurrentDirectoryA(sizeof(cd), cd))
#else
    char cd[PATH_MAX];
    if (!getcwd(cd, sizeof(cd)))
#endif
    {
        Printf(RED "ERROR: Cannot get the working directory!\n");
        return false;
    }

    configFile = fs::path(cd) / fs::path(config);

    for (fs::path& path : includeDirs)
        path = configFile.parent_path() / path;

    return true;
}

int AddLocalDefine(struct argparse* self, const struct argparse_option* option)
{ ((ConfigLine*)(option->data))->defines.push_back(*(const char**)option->value); UNUSED(self); return 0; }

bool ConfigLine::Parse(int32_t argc, const char** argv)
{
    source = argv[0];

    const char* unused = nullptr; // storage for the callback
    struct argparse_option options[] = {
        OPT_STRING('T', "profile", &profile, "Shader profile", nullptr, 0, 0),
        OPT_STRING('E', "entryPoint", &entryPoint, "(Optional) entry point", nullptr, 0, 0),
        OPT_STRING('D', "define", &unused, "(Optional) define(s) in forms 'M=value' or 'M'", AddLocalDefine, (intptr_t)this, 0),
        OPT_STRING('o', "output", &outputDir, "(Optional) output subdirectory", nullptr, 0, 0),
        OPT_INTEGER('O', "optimization", &optimizationLevel, "(Optional) optimization level", nullptr, 0, 0),
        OPT_END(),
    };

    static const char* usages[] = {
        "path/to/shader -T profile [-E entry -O{0|1|2|3} -o \"output/subdirectory\" -D DEF1={0,1} -D DEF2={0,1,2} -D DEF3 ...]",
        nullptr
    };

    struct argparse argparse;
    argparse_init(&argparse, options, usages, 0);
    argparse_describe(&argparse, nullptr, "\nConfiguration options for a shader");
    argparse_parse(&argparse, argc, argv);

    if (!profile)
    {
        Printf(RED "ERROR: Shader target not specified!\n");
        return false;
    }

    return true;
}

//=====================================================================================================================
// FXC/DXC API
//=====================================================================================================================
#ifdef _WIN32

void TokenizeDefineStrings(vector<string>& in, vector<D3D_SHADER_MACRO>& out)
{
    if (in.empty())
        return;

    out.reserve(out.size() + in.size());
    for (const string& defineString : in)
    {
        D3D_SHADER_MACRO& define = out.emplace_back();
        char* s = (char*)defineString.c_str(); // IMPORTANT: "defineString" gets split into tokens divided by '\0'
        define.Name = strtok(s, "=");
        define.Definition = strtok(nullptr, "=");
    }
}

class FxcIncluder : public ID3DInclude
{
public:
    FxcIncluder(const wchar_t* file)
    {
        baseDir = fs::path(file).parent_path();

        includeDirs.reserve(g_Options.includeDirs.size() + 8);
        for (const fs::path& path : g_Options.includeDirs)
            includeDirs.push_back(path);

        includeDirs.push_back(baseDir);
    }

    ~FxcIncluder()
    {}

    STDMETHOD(Open)(THIS_ D3D_INCLUDE_TYPE includeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID *ppData, UINT *pBytes)
    {
        UNUSED(includeType);
        UNUSED(pParentData);

        *ppData = 0;
        *pBytes = 0;

        // Load file
        fs::path name = fs::path(pFileName);
        for (const fs::path& currentPath : includeDirs)
        {
            fs::path file = currentPath / name;

            FILE* stream = fopen(file.string().c_str(), "rb");
            if (stream)
            {
                // Add the path to this file to the current include stack so that any sub-includes would be relative to this path
                fs::path path = file.parent_path().lexically_normal();
                includeDirs.push_back(path);

                uint32_t len = GetFileLength(stream);

                char* buf = (char*)malloc(len);
                if (!buf)
                    return E_FAIL;

                fread(buf, 1, len, stream);
                fclose(stream);

                *ppData = buf;
                *pBytes = len;

                return S_OK;
            }
        }

        return E_FAIL;
    }

    STDMETHOD(Close)(THIS_ LPCVOID pData)
    {
        if (pData)
        {
            // Pop the path for the innermost included file from the include stack
            includeDirs.pop_back();

            // Release the data
            free((void*)pData);
        }

        return S_OK;
    }

private:
    fs::path baseDir;
    vector<fs::path> includeDirs;
};

void FxcCompile()
{
    static const uint32_t optimizationLevelRemap[] = {
        D3DCOMPILE_SKIP_OPTIMIZATION,
        D3DCOMPILE_OPTIMIZATION_LEVEL1,
        D3DCOMPILE_OPTIMIZATION_LEVEL2,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
    };

    vector<D3D_SHADER_MACRO> optionsDefines;
    vector<string> tokenizedDefines = g_Options.defines;
    TokenizeDefineStrings(tokenizedDefines, optionsDefines);

    while (!g_Terminate)
    {
        // Getting a task in the current thread
        TaskData taskData;
        {
            lock_guard<mutex> guard(g_TaskMutex);
            if (g_TaskData.empty())
                return;

            taskData = g_TaskData.back();
            g_TaskData.pop_back();
        }

        // Tokenize DXBC defines
        vector<D3D_SHADER_MACRO> defines = optionsDefines;
        TokenizeDefineStrings(taskData.defines, defines);
        defines.push_back({nullptr, nullptr});

        // Args
        uint32_t compilerFlags = (g_Options.pdb ? (D3DCOMPILE_DEBUG | D3DCOMPILE_DEBUG_NAME_FOR_BINARY) : 0) |
            (g_Options.allResourcesBound ? D3DCOMPILE_ALL_RESOURCES_BOUND : 0) |
            (g_Options.warningsAreErrors ? D3DCOMPILE_WARNINGS_ARE_ERRORS : 0) |
            (g_Options.matrixRowMajor ? D3DCOMPILE_PACK_MATRIX_ROW_MAJOR : 0) |
            optimizationLevelRemap[taskData.optimizationLevel];

        // Compiling the shader
        fs::path sourceFile = g_Options.configFile.parent_path() / g_Options.sourceDir / taskData.source;

        FxcIncluder fxcIncluder(sourceFile.wstring().c_str());
        string profile = taskData.profile + "_5_0";

        ComPtr<ID3DBlob> codeBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompileFromFile(
            sourceFile.wstring().c_str(),
            defines.data(),
            &fxcIncluder,
            taskData.entryPoint.c_str(),
            profile.c_str(),
            compilerFlags, 0,
            &codeBlob,
            &errorBlob);

        bool isSucceeded = SUCCEEDED(hr) && codeBlob;

        if (g_Terminate)
            break;

        // Dump PDB
        if (isSucceeded && g_Options.pdb)
        {
            // Retrieve the debug info part of the shader
            ComPtr<ID3DBlob> pdb;
            D3DGetBlobPart(codeBlob->GetBufferPointer(), codeBlob->GetBufferSize(), D3D_BLOB_PDB, 0, &pdb);

            // Retrieve the suggested name for the debug data file
            ComPtr<ID3DBlob> pdbName;
            D3DGetBlobPart(codeBlob->GetBufferPointer(), codeBlob->GetBufferSize(), D3D_BLOB_DEBUG_NAME, 0, &pdbName);

            // This struct represents the first four bytes of the name blob
            struct ShaderDebugName
            {
                uint16_t Flags;       // Reserved, must be set to zero
                uint16_t NameLength;  // Length of the debug name, without null terminator
                                      // Followed by NameLength bytes of the UTF-8-encoded name
                                      // Followed by a null terminator
                                      // Followed by [0-3] zero bytes to align to a 4-byte boundary
            };

            auto pDebugNameData = (const ShaderDebugName*)(pdbName->GetBufferPointer());
            auto pName = (const char*)(pDebugNameData + 1);

            string file = fs::path(taskData.outputFileWithoutExt).parent_path().string() + "/" + PDB_DIR + "/" + pName;
            FILE* fp = fopen(file.c_str(), "wb");
            if (fp)
            {
                fwrite(pdb->GetBufferPointer(), pdb->GetBufferSize(), 1, fp);
                fclose(fp);
            }
        }

        // Strip reflection
        ComPtr<ID3DBlob> strippedBlob;
        if (g_Options.stripReflection && isSucceeded)
        {
            D3DStripShader(codeBlob->GetBufferPointer(), codeBlob->GetBufferSize(), D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO, &strippedBlob);
            codeBlob = strippedBlob;
        }

        // Dump output
        if (isSucceeded)
            DumpShader(taskData, (uint8_t*)codeBlob->GetBufferPointer(), codeBlob->GetBufferSize());

        // Update progress
        UpdateProgress(taskData, isSucceeded, errorBlob ? (char*)errorBlob->GetBufferPointer() : nullptr);

        // Terminate if this shader failed and "--continue" is not set
        if (g_Terminate)
            break;
    }
}

void DxcCompile()
{
    static const wchar_t* optimizationLevelRemap[] = {
        DXC_ARG_SKIP_OPTIMIZATIONS,
        DXC_ARG_OPTIMIZATION_LEVEL1,
        DXC_ARG_OPTIMIZATION_LEVEL2,
        DXC_ARG_OPTIMIZATION_LEVEL3,
    };

    // Gather SPIRV register shifts once
    static const wchar_t* regShiftArgs[] = {
        L"-fvk-s-shift",
        L"-fvk-t-shift",
        L"-fvk-b-shift",
        L"-fvk-u-shift",
    };

    vector<wstring> regShifts;
    for (uint32_t reg = 0; reg < 4; reg++)
    {
        for (uint32_t space = 0; space < SPIRV_SPACES_NUM; space++)
        {
            wchar_t buf[64];

            regShifts.push_back(regShiftArgs[reg]);

            swprintf(buf, COUNT_OF(buf), L"%u", (&g_Options.sRegShift)[reg]);
            regShifts.push_back(wstring(buf));

            swprintf(buf, COUNT_OF(buf), L"%u", space);
            regShifts.push_back(wstring(buf));
        }
    }

    // TODO: is a global instance thread safe?
    ComPtr<IDxcCompiler3> dxcCompiler;
    HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
    if (FAILED(hr))
        return;

    ComPtr<IDxcUtils> dxcUtils;
    hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils));
    if (FAILED(hr))
        return;

    while (!g_Terminate)
    {
        // Getting a task in the current thread
        TaskData taskData;
        {
            lock_guard<mutex> guard(g_TaskMutex);
            if (g_TaskData.empty())
                return;

            taskData = g_TaskData.back();
            g_TaskData.pop_back();
        }

        // Compiling the shader
        fs::path sourceFile = g_Options.configFile.parent_path() / g_Options.sourceDir / taskData.source;
        wstring wsourceFile = sourceFile.wstring();

        ComPtr<IDxcBlob> codeBlob;
        ComPtr<IDxcBlobEncoding> errorBlob;
        bool isSucceeded = false;

        ComPtr<IDxcBlobEncoding> sourceBlob;
        hr = dxcUtils->LoadFile(wsourceFile.c_str(), nullptr, &sourceBlob);

        if (SUCCEEDED(hr))
        {
            vector<wstring> args;
            args.reserve(16 + (g_Options.defines.size() + taskData.defines.size() + g_Options.includeDirs.size()) * 2
                + (g_Options.platform == SPIRV ? regShifts.size() + g_Options.spirvExtensions.size() : 0));

            // Source file
            args.push_back(wsourceFile);

            // Profile
            args.push_back(L"-T");
            args.push_back(AnsiToWide(taskData.profile + "_" + g_Options.shaderModel));

            // Entry point
            args.push_back(L"-E");
            args.push_back(AnsiToWide(taskData.entryPoint));

            // Defines
            for (const string& define : g_Options.defines)
            {
                args.push_back(L"-D");
                args.push_back(AnsiToWide(define));
            }
            for (const string& define : taskData.defines)
            {
                args.push_back(L"-D");
                args.push_back(AnsiToWide(define));
            }

            // Include directories
            for (const fs::path& path : g_Options.includeDirs)
            {
                args.push_back(L"-I");
                args.push_back(path.wstring());
            }

            // Args
            args.push_back(optimizationLevelRemap[taskData.optimizationLevel]);

            uint32_t shaderModelIndex = (g_Options.shaderModel[0] - '0') * 10 + (g_Options.shaderModel[2] - '0');
            if (shaderModelIndex >= 62)
                args.push_back(L"-enable-16bit-types");

            if (g_Options.warningsAreErrors)
                args.push_back(DXC_ARG_WARNINGS_ARE_ERRORS);

            if (g_Options.allResourcesBound)
                args.push_back(DXC_ARG_ALL_RESOURCES_BOUND);

            if (g_Options.matrixRowMajor)
                args.push_back(DXC_ARG_PACK_MATRIX_ROW_MAJOR);

            if (g_Options.hlsl2021)
            {
                args.push_back(L"-HV");
                args.push_back(L"2021");
            }

            if (g_Options.pdb)
            {
                // TODO: for SPIRV PDB can only be embedded, GetOutput(DXC_OUT_PDB) silently fails...
                args.push_back(L"-Zi");
                args.push_back(L"-Zsb"); // only binary code affects hash
            }

            if (g_Options.platform == SPIRV)
            {
                args.push_back(L"-spirv");
                args.push_back(wstring(L"-fspv-target-env=vulkan") + AnsiToWide(g_Options.vulkanVersion));

                for (const string& ext : g_Options.spirvExtensions)
                    args.push_back(wstring(L"-fspv-extension=") + AnsiToWide(ext));

                for (const wstring& arg : regShifts)
                    args.push_back(arg);
            }
            else // Not supported by SPIRV gen
            {
                if (g_Options.stripReflection)
                    args.push_back(L"-Qstrip_reflect");
            }

            // Debug output
            if (g_Options.verbose)
            {
                wstringstream cmd;
                for (const wstring& arg : args)
                {
                    cmd << arg;
                    cmd << L" ";
                }

                Printf(WHITE "%ls\n", cmd.str().c_str());
            }

            // Now that args are finalized, get their C-string pointers into a vector
            vector<const wchar_t*> argPointers;
            argPointers.reserve(args.size());
            for (const wstring& arg : args)
                argPointers.push_back(arg.c_str());

            // Compiling the shader
            DxcBuffer sourceBuffer = {};
            sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
            sourceBuffer.Size = sourceBlob->GetBufferSize();

            ComPtr<IDxcIncludeHandler> pDefaultIncludeHandler;
            dxcUtils->CreateDefaultIncludeHandler(&pDefaultIncludeHandler);

            ComPtr<IDxcResult> dxcResult;
            hr = dxcCompiler->Compile(&sourceBuffer, argPointers.data(), (uint32_t)args.size(), pDefaultIncludeHandler.Get(), IID_PPV_ARGS(&dxcResult));

            if (SUCCEEDED(hr))
                dxcResult->GetStatus(&hr);

            if (dxcResult)
            {
                dxcResult->GetResult(&codeBlob);
                dxcResult->GetErrorBuffer(&errorBlob);
            }

            isSucceeded = SUCCEEDED(hr) && codeBlob;

            // Dump PDB
            if (isSucceeded && g_Options.pdb)
            {
                ComPtr<IDxcBlob> pdb;
                ComPtr<IDxcBlobUtf16> pdbName;
                if (SUCCEEDED(dxcResult->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(&pdb), &pdbName)))
                {
                    wstring file = fs::path(taskData.outputFileWithoutExt).parent_path().wstring() + L"/" + _L(PDB_DIR) + L"/" + wstring(pdbName->GetStringPointer());
                    FILE* fp = _wfopen(file.c_str(), L"wb");
                    if (fp)
                    {
                        fwrite(pdb->GetBufferPointer(), pdb->GetBufferSize(), 1, fp);
                        fclose(fp);
                    }
                }
            }
        }

        if (g_Terminate)
            break;

        // Dump output
        if (isSucceeded)
            DumpShader(taskData, (uint8_t*)codeBlob->GetBufferPointer(), codeBlob->GetBufferSize());

        // Update progress
        UpdateProgress(taskData, isSucceeded, errorBlob ? (char*)errorBlob->GetBufferPointer() : nullptr);
    }
}

#endif

//=====================================================================================================================
// EXE
//=====================================================================================================================

void ExeCompile()
{
    static const char* optimizationLevelRemap[] = {
        " -Od",
        " -O1",
        " -O2",
        " -O3",
    };

    while (!g_Terminate)
    {
        // Getting a task in the current thread
        TaskData taskData;
        {
            lock_guard<mutex> guard(g_TaskMutex);
            if (g_TaskData.empty())
                return;

            taskData = g_TaskData.back();
            g_TaskData.pop_back();
        }

        // Building command line
        ostringstream cmd;
        {
            #ifdef _WIN32 // workaround for Windows
                cmd << "%COMPILER% -nologo ";
            #else
                cmd << "$COMPILER -nologo ";
            #endif

            // Source file
            fs::path sourceFile = g_Options.configFile.parent_path() / g_Options.sourceDir / taskData.source;
            cmd << EscapePath(sourceFile.string());

            // Output file
            string outputFile = taskData.outputFileWithoutExt + g_OutputExt;
            if (g_Options.binary || g_Options.IsBlob())
                cmd << " -Fo " << EscapePath(outputFile);
            if (g_Options.header)
            {
                string name = GetShaderName(taskData.outputFileWithoutExt);

                cmd << " -Fh " << EscapePath(outputFile) << ".h";
                cmd << " -Vn " << name;
            }

            // Profile
            string profile = taskData.profile + "_";
            if (g_Options.platform == DXBC)
                profile += "5_0";
            else
                profile += g_Options.shaderModel;
            cmd << " -T " << profile;

            // Entry point
            cmd << " -E " << taskData.entryPoint;

            // Defines
            for (const string& define : taskData.defines)
                cmd << " -D " << define;

            for (const string& define : g_Options.defines)
                cmd << " -D " << define;

            // Include directories
            for (const fs::path& dir : g_Options.includeDirs)
                cmd << " -I " << EscapePath(dir.string());

            // Args
            cmd << optimizationLevelRemap[taskData.optimizationLevel];

            uint32_t shaderModelIndex = (g_Options.shaderModel[0] - '0') * 10 + (g_Options.shaderModel[2] - '0');
            if (g_Options.platform != DXBC && shaderModelIndex >= 62)
                cmd << " -enable-16bit-types";

            if (g_Options.warningsAreErrors)
                cmd << " -WX";

            if (g_Options.allResourcesBound)
                cmd << " -all_resources_bound";

            if (g_Options.matrixRowMajor)
                cmd << " -Zpr";

            if (g_Options.hlsl2021)
                cmd << " -HV 2021";

            if (g_Options.pdb)
                cmd << " -Zi -Zsb"; // only binary affects hash

            if (g_Options.platform == SPIRV)
            {
                cmd << " -spirv";

                cmd << " -fspv-target-env=vulkan" << g_Options.vulkanVersion;

                for (const string& ext : g_Options.spirvExtensions)
                    cmd << " -fspv-extension=" << ext;

                for (uint32_t space = 0; space < SPIRV_SPACES_NUM; space++)
                {
                    cmd << " -fvk-s-shift " << g_Options.sRegShift << " " << space;
                    cmd << " -fvk-t-shift " << g_Options.tRegShift << " " << space;
                    cmd << " -fvk-b-shift " << g_Options.bRegShift << " " << space;
                    cmd << " -fvk-u-shift " << g_Options.uRegShift << " " << space;
                }
            }
            else // Not supported by SPIRV gen
            {
                if (g_Options.stripReflection)
                    cmd << " -Qstrip_reflect";

                if (g_Options.pdb)
                {
                    fs::path pdbPath = fs::path(outputFile).parent_path() / PDB_DIR;
                    cmd << " -Fd " << EscapePath(pdbPath.string() + "/"); // only binary code affects hash
                }
            }
        }

        cmd << " 2>&1";

        // Debug output
        if (g_Options.verbose)
            Printf(WHITE "%s\n", cmd.str().c_str());

        // Compiling the shader
        ostringstream msg;
        FILE* pipe = popen(cmd.str().c_str(), "r");

        bool isSucceeded = false;
        if (pipe)
        {
            char buf[1024];
            while (fgets(buf, sizeof(buf), pipe))
            {
                // Ignore useless unmutable FXC message
                if (strstr(buf, "compilation object save succeeded"))
                    continue;

                msg << buf;
            }

            isSucceeded = pclose(pipe) == 0;
        }

        // Update progress
        UpdateProgress(taskData, isSucceeded, msg.str().c_str());
    }
}

//=====================================================================================================================
// MAIN
//=====================================================================================================================

bool GetHierarchicalUpdateTime(const fs::path& file, list<fs::path>& callStack, fs::file_time_type& outTime)
{
    static const basic_regex<char> includePattern("\\s*#include\\s+[\"<]([^>\"]+)[>\"].*");

    auto found = g_HierarchicalUpdateTimes.find(file);
    if (found != g_HierarchicalUpdateTimes.end())
    {
        outTime = found->second;

        return true;
    }

    ifstream stream(file);
    if (!stream.is_open())
    {
        Printf(RED "ERROR: Can't open file '%s', included in:\n", PathToString(file).c_str());
        for (const fs::path& otherFile : callStack)
            Printf(RED "\t%s\n", PathToString(otherFile).c_str());

        return false;
    }

    callStack.push_front(file);

    fs::path path = file.parent_path();
    fs::file_time_type hierarchicalUpdateTime = fs::last_write_time(file);

    for (string line; getline(stream, line);)
    {
        match_results<const char*> matchResult;
        regex_match(line.c_str(), matchResult, includePattern);
        if (matchResult.empty())
            continue;

        fs::path includeName = string(matchResult[1]);
        if (find(g_Options.relaxedIncludes.begin(), g_Options.relaxedIncludes.end(), includeName) != g_Options.relaxedIncludes.end())
            continue;

        bool isFound = false;
        fs::path includeFile = path / includeName;
        if (fs::exists(includeFile))
            isFound = true;
        else
        {
            for (const fs::path& includePath : g_Options.includeDirs)
            {
                includeFile = includePath / includeName;
                if (fs::exists(includeFile))
                {
                    isFound = true;
                    break;
                }
            }
        }

        if (!isFound)
        {
            Printf(RED "ERROR: Can't find include file '%s', included in:\n", PathToString(includeName).c_str());
            for (const fs::path& otherFile : callStack)
                Printf(RED "\t%s\n", PathToString(otherFile).c_str());

            return false;
        }

        fs::file_time_type dependencyTime;
        if (!GetHierarchicalUpdateTime(includeFile, callStack, dependencyTime))
            return false;

        hierarchicalUpdateTime = max(dependencyTime, hierarchicalUpdateTime);
    }

    callStack.pop_front();

    g_HierarchicalUpdateTimes[file] = hierarchicalUpdateTime;
    outTime = hierarchicalUpdateTime;

    return true;
}

bool ProcessConfigLine(uint32_t lineIndex, const string& line, const fs::file_time_type& configTime)
{
    // Tokenize
    string lineCopy = line;
    vector<const char*> tokens;
    TokenizeConfigLine((char*)lineCopy.c_str(), tokens);

    // Parse config line
    ConfigLine configLine;
    if (!configLine.Parse((int32_t)tokens.size(), tokens.data()))
    {
        Printf(RED "%s(%u,0): ERROR: Can't parse config line!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);

        return false;
    }

    // DXBC: skip unsupported profiles
    string profile = configLine.profile;
    if (g_Options.platform == DXBC && (profile == "lib" || profile == "ms" || profile == "as"))
        return true;

    // Concatenate define strings, i.e. to get something, like: "A=1 B=0 C"
    string combinedDefines = "";
    for (size_t i = 0; i < configLine.defines.size(); i++)
    {
        combinedDefines += configLine.defines[i];
        if (i != configLine.defines.size() - 1 )
            combinedDefines += " ";
    }

    // Compiled shader name
    fs::path shaderName = RemoveLeadingDotDots(configLine.source);
    shaderName.replace_extension("");
    if (g_Options.flatten || configLine.outputDir) // Specifying -o <path> for a shader removes the original path
        shaderName = shaderName.filename();
    if (strcmp(configLine.entryPoint, "main"))
        shaderName += "_" + string(configLine.entryPoint);

    // Compiled permutation name
    fs::path permutationName = shaderName;
    if (!configLine.defines.empty())
    {
        uint32_t permutationHash = HashToUint(hash<string>()(combinedDefines));

        char buf[16];
        snprintf(buf, sizeof(buf), "_%08X", permutationHash);

        permutationName += buf;
    }

    // Output directory
    fs::path outputDir = g_Options.outputDir;
    if (configLine.outputDir)
        outputDir /= configLine.outputDir;

    // Create intermediate output directories
    bool force = g_Options.force;
    fs::path endPath = outputDir / shaderName.parent_path();
    if (g_Options.pdb)
        endPath /= PDB_DIR;
    if (endPath.string() != "" && !fs::exists(endPath))
    {
        fs::create_directories(endPath);
        force = true;
    }

    // Early out if no changes detected
    fs::file_time_type zero; // constructor sets to 0
    fs::file_time_type outputTime = zero;

    {
        fs::path outputFile = outputDir / permutationName;

        outputFile += g_OutputExt;
        if (g_Options.binary)
        {
            force |= !fs::exists(outputFile);
            if (!force)
            {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }

        outputFile += ".h";
        if (g_Options.header)
        {
            force |= !fs::exists(outputFile);
            if (!force)
            {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }
    }

    {
        fs::path outputFile = outputDir / shaderName;

        outputFile += g_OutputExt;
        if (g_Options.binaryBlob)
        {
            force |= !fs::exists(outputFile);
            if (!force)
            {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }

        outputFile += ".h";
        if (g_Options.headerBlob)
        {
            force |= !fs::exists(outputFile);
            if (!force)
            {
                if (outputTime == zero)
                    outputTime = fs::last_write_time(outputFile);
                else
                    outputTime = min(outputTime, fs::last_write_time(outputFile));
            }
        }
    }

    if (!force)
    {
        list<fs::path> callStack;
        fs::file_time_type sourceTime;
        fs::path sourceFile = g_Options.configFile.parent_path() / g_Options.sourceDir / configLine.source;
        if (!GetHierarchicalUpdateTime(sourceFile, callStack, sourceTime))
            return false;

        sourceTime = max(sourceTime, configTime);
        if (outputTime > sourceTime)
            return true;
    }

    // Prepare a task
    string outputFileWithoutExt = PathToString(outputDir / permutationName);
    uint32_t optimizationLevel = configLine.optimizationLevel == USE_GLOBAL_OPTIMIZATION_LEVEL ? g_Options.optimizationLevel : configLine.optimizationLevel;
    optimizationLevel = min(optimizationLevel, 3u);

    TaskData& taskData = g_TaskData.emplace_back();
    taskData.source = configLine.source;
    taskData.entryPoint = configLine.entryPoint;
    taskData.profile = configLine.profile;
    taskData.combinedDefines = combinedDefines;
    taskData.outputFileWithoutExt = outputFileWithoutExt;
    taskData.defines = configLine.defines;
    taskData.optimizationLevel = optimizationLevel;

    // Gather blobs
    if (g_Options.IsBlob())
    {
        string blobName = PathToString(outputDir / shaderName);
        vector<BlobEntry>& entries = g_ShaderBlobs[blobName];

        BlobEntry entry;
        entry.permutationFileWithoutExt = outputFileWithoutExt;
        entry.combinedDefines = combinedDefines;
        entries.push_back(entry);
    }

    return true;
}

bool ExpandPermutations(uint32_t lineIndex, const string& line, const fs::file_time_type& configTime)
{
    size_t opening = line.find('{');
    if (opening == string::npos)
        return ProcessConfigLine(lineIndex, line, configTime);

    size_t closing = line.find('}', opening);
    if (closing == string::npos)
    {
        Printf(RED "%s(%u,0): ERROR: Missing '}'!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);

        return false;
    }

    size_t current = opening + 1;
    while (true)
    {
        size_t comma = line.find(',', current);
        if (comma == string::npos || comma > closing)
            comma = closing;

        string newConfig = line.substr(0, opening) + line.substr(current, comma - current) + line.substr(closing + 1);
        if (!ExpandPermutations(lineIndex, newConfig, configTime))
            return false;

        current = comma + 1;
        if (comma >= closing)
            break;
    }

    return true;
}

bool CreateBlob(const string& blobName, const vector<BlobEntry>& entries, bool useTextOutput)
{
    // Create output file
    string outputFile = blobName;
    outputFile += g_OutputExt;
    if (useTextOutput)
        outputFile += ".h";

    DataOutputContext outputContext(outputFile.c_str(), useTextOutput);
    if (!outputContext.stream)
    {
        Printf(RED "ERROR: Can't open output file '%s'!\n", outputFile.c_str());

        return false;
    }

    if (useTextOutput)
    {
        string name = GetShaderName(blobName);
        outputContext.WriteTextPreamble(name.c_str());
    }

    ShaderMake::WriteFileCallback writeFileCallback = useTextOutput
        ? &DataOutputContext::WriteDataAsTextCallback
        : &DataOutputContext::WriteDataAsBinaryCallback;

    // Write "blob" header
    if (!ShaderMake::WriteFileHeader(writeFileCallback, &outputContext))
    {
        Printf(RED "ERROR: Failed to write into output file '%s'!\n", outputFile.c_str());

        return false;
    }

    bool success = true;

    // Collect individual permutations
    for (const BlobEntry& entry : entries)
    {
        // Open compiled permutation file
        string file = entry.permutationFileWithoutExt + g_OutputExt;
        FILE* sourceStream = fopen(file.c_str(), "rb");
        if (!sourceStream)
        {
            Printf(RED "ERROR: Can't open file source '%s'!\n", file.c_str());

            return false;
        }

        uint32_t sourceSize = GetFileLength(sourceStream);

        // Warn if the file is suspiciously large
        if (sourceSize > (64 << 20)) // > 64Mb
            Printf(YELLOW "WARNING: Binary file '%s' is too large!\n", file.c_str());

        // Allocate memory foe the whole file
        void* buffer = malloc(sourceSize);
        if (buffer)
        {
            // Read the source file
            size_t bytesRead = fread(buffer, 1, sourceSize, sourceStream);
            if (bytesRead == sourceSize)
            {
                if (!ShaderMake::WritePermutation(writeFileCallback, &outputContext, entry.combinedDefines, buffer, sourceSize))
                {
                    Printf(RED "ERROR: Failed to write a shader permutation into '%s'!\n", outputFile.c_str());
                    success = false;
                }
            }
            else
            {
                Printf(RED "ERROR: Failed to read %llu bytes from '%s'!\n", sourceSize, file.c_str());
                success = false;
            }

            free(buffer);
        }
        else if (sourceSize)
        {
            Printf(RED "ERROR: Can't allocate %u bytes!\n", sourceSize);
            success = false;
        }
        else
        {
            Printf(RED "ERROR: Binary file '%s' is empty!\n", file.c_str());
            success = false;
        }

        fclose(sourceStream);

        if (!success)
            break;
    }

    if (useTextOutput)
        outputContext.WriteTextEpilog();

    return success;
}

void RemoveIntermediateBlobFiles(const vector<BlobEntry>& entries)
{
    for (const BlobEntry& entry : entries)
    {
        string file = entry.permutationFileWithoutExt + g_OutputExt;
        fs::remove(file);
    }
}

void SignalHandler(int32_t sig)
{
    UNUSED(sig);

    g_Terminate = true;

    Printf(RED "Aborting...\n");
}

int32_t main(int32_t argc, const char** argv)
{
    // Init timer
    Timer_Init();
    uint64_t start = Timer_GetTicks();

    // Set signal handler
    signal(SIGINT, SignalHandler);
#ifdef _WIN32
    signal(SIGBREAK, SignalHandler);
#endif

    // Parse command line
    const char* self = argv[0];
    if (!g_Options.Parse(argc, argv))
        return 1;

    // Set envvar
    char envBuf[1024];
    if (!g_Options.useAPI)
    {
        #ifdef _WIN32 // workaround for Windows
            snprintf(envBuf, sizeof(envBuf), "COMPILER=\"%s\"", g_Options.compiler);
        #else
            snprintf(envBuf, sizeof(envBuf), "COMPILER=%s", g_Options.compiler);
        #endif

        if (putenv(envBuf) != 0)
            return 1;
    }

#ifdef _WIN32
    // Setup a directory where to look for "dxcompiler" first
    fs::path compilerDir = fs::path(g_Options.compiler).parent_path();
    if (g_Options.platform != DXBC && compilerDir != "")
        SetDllDirectoryA(compilerDir.string().c_str());
#endif

    { // Gather shader permutations
        fs::file_time_type configTime = fs::last_write_time(g_Options.configFile);
        configTime = max(configTime, fs::last_write_time(self));

        ifstream configStream(g_Options.configFile);

        string line;
        line.reserve(256);

        vector<bool> blocks;
        blocks.push_back(true);

        for (uint32_t lineIndex = 0; getline(configStream, line); lineIndex++)
        {
            TrimConfigLine(line);

            // Skip an empty or commented line
            if (line.empty() || line[0] == '\n' || (line[0] == '/' && line[1] == '/'))
                continue;

            // TODO: preprocessor supports "#ifdef MACRO / #if 1 / #if 0", "#else" and "#endif"
            size_t pos = line.find("#ifdef");
            if (pos != string::npos)
            {
                pos += 6;
                pos += line.substr(pos).find_first_not_of(' ');

                string define = line.substr(pos);
                bool state = blocks.back() && find(g_Options.defines.begin(), g_Options.defines.end(), define) != g_Options.defines.end();

                blocks.push_back(state);
            }
            else if (line.find("#if 1") != string::npos)
                blocks.push_back(blocks.back());
            else if (line.find("#if 0") != string::npos)
                blocks.push_back(false);
            else if (line.find("#endif") != string::npos)
            {
                if (blocks.size() == 1)
                    Printf(RED "%s(%u,0): ERROR: Unexpected '#endif'!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);
                else
                    blocks.pop_back();
            }
            else if (line.find("#else") != string::npos)
            {
                if (blocks.size() < 2)
                    Printf(RED "%s(%u,0): ERROR: Unexpected '#else'!\n", PathToString(g_Options.configFile).c_str(), lineIndex + 1);
                else if (blocks[blocks.size() - 2])
                    blocks.back() = !blocks.back();
            }
            else if (blocks.back())
            {
                if (!ExpandPermutations(lineIndex, line, configTime))
                    return 1;
            }
        }
    }

    // Process tasks
    if (!g_TaskData.empty())
    {
        Printf(WHITE "Using compiler: %s\n", g_Options.compiler);

        g_OriginalTaskCount = (uint32_t)g_TaskData.size();
        g_ProcessedTaskCount = 0;
        g_FailedTaskCount = 0;

        uint32_t threadsNum = max(g_Options.serial ? 1 : thread::hardware_concurrency(), 1u);

        vector<thread> threads(threadsNum);
        for (uint32_t i = 0; i < threadsNum; i++)
        {
            if (!g_Options.useAPI)
                threads[i] = thread(ExeCompile);
#ifdef WIN32
            else if (g_Options.platform == DXBC)
                threads[i] = thread(FxcCompile);
            else
                threads[i] = thread(DxcCompile);
#endif
        }

        for (uint32_t i = 0; i < threadsNum; i++)
            threads[i].join();

        // Dump shader blobs
        for (const auto& [blobName, blobEntries] : g_ShaderBlobs)
        {
            // If a blob would contain one entry with no defines, just skip it:
            // the individual file's output name is the same as the blob, and we're done here.
            if (blobEntries.size() == 1 && blobEntries[0].combinedDefines.empty())
                continue;

            // Validate that the blob doesn't contain any shaders with empty defines.
            // In such case, that individual shader's output file is the same as the blob output file, which wouldn't work.
            // We could detect this condition earlier and work around it by renaming the shader output file, if necessary.
            bool invalidEntry = false;
            for (const auto& entry : blobEntries)
            {
                if (entry.combinedDefines.empty())
                {
                    const string blobBaseName = fs::path(blobName).stem().generic_string();
                    Printf(RED "ERROR: Cannot create a blob for shader %s where some permutation(s) have no definitions!",
                        blobBaseName.c_str());
                    invalidEntry = true;
                    break;
                }
            }

            if (invalidEntry)
            {
                if (g_Options.continueOnError)
                    continue;

                return 1;
            }

            if (g_Options.binaryBlob)
            {
                bool result = CreateBlob(blobName, blobEntries, false);
                if (!result && !g_Options.continueOnError)
                    return 1;
            }

            if (g_Options.headerBlob)
            {
                bool result = CreateBlob(blobName, blobEntries, true);
                if (!result && !g_Options.continueOnError)
                    return 1;
            }

            if (!g_Options.binary)
                RemoveIntermediateBlobFiles(blobEntries);
        }

        // Report failed tasks
        if (g_FailedTaskCount)
            Printf(YELLOW "WARNING: %u task(s) failed to complete!\n", g_FailedTaskCount.load());
        else
            Printf(WHITE "%d task(s) completed successfully.\n", g_OriginalTaskCount);

        uint64_t end = Timer_GetTicks();
        Printf(WHITE "Elapsed time %.2f ms\n\n", Timer_ConvertTicksToMilliseconds(end - start));
    }
    else
        Printf(WHITE "All %s shaders are up to date.\n", g_Options.platformName);

    return (g_Terminate || g_FailedTaskCount) ? 1 : 0;
}
