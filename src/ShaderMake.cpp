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

#ifdef _WIN32
    #include "ComPtr.h"
    #include <d3dcompiler.h> // FXC
    #include <dxcapi.h> // DXC
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
    bool isBinaryNeeded = false;
    bool isHeaderNeeded = false;
    bool isBlobNeeded = false;
    bool continueOnError = false;
    bool warningsAreErrors = false;
    bool allResourcesBound = false;
    bool pdb = false;
    bool stripReflection = false;
    bool matrixRowMajor = false;
    bool verbose = false;
    bool colorize = false;
    bool useAPI = false;

    bool Parse(int32_t argc, const char** argv);
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
    fs::path compiledPermutationFileWithoutExt;
    string permutation;
};

map<fs::path, fs::file_time_type> g_HierarchicalUpdateTimes;
map<string, vector<BlobEntry>> g_ShaderBlobs;
vector<TaskData> g_TaskData;
atomic<uint32_t> g_ProcessedTaskCount;
atomic<bool> g_Terminate = false;
atomic<uint32_t> g_FailedTaskCount = 0;
uint32_t g_OriginalTaskCount;
Options g_Options;
mutex g_TaskMutex;
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

uint32_t GetFileLength(FILE* f)
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

    const uint32_t pos = ftell(f);
    fseek(f, 0, SEEK_END);
    const uint32_t len = ftell(f);
    fseek(f, pos, SEEK_SET);

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

void DumpBinaryAndHeader(const TaskData& taskData, const uint8_t* data, size_t dataSize)
{
    string outputFile = taskData.outputFileWithoutExt + g_OutputExt;

    // Binary output
    if (g_Options.isBinaryNeeded || g_Options.isBlobNeeded)
    {
        FILE* stream = fopen(outputFile.c_str(), "wb");
        if (stream)
        {
            fwrite(data, 1, dataSize, stream);
            fclose(stream);
        }
    }

    // Header output
    if (g_Options.isHeaderNeeded)
    {
        outputFile += ".h";

        FILE* stream = fopen(outputFile.c_str(), "w");
        if (stream)
        {
            uint32_t n = 129;

            fs::path path = taskData.outputFileWithoutExt;
            string name = path.filename().string();
            replace(name.begin(), name.end(), '.', '_');

            fprintf(stream, "const uint8_t g_%s_%s[] = {", name.c_str(), g_PlatformExts[g_Options.platform] + 1);

            for (size_t i = 0; i < dataSize; i++)
            {
                uint8_t d = data[i];

                if (n > 128)
                {
                    fprintf(stream, "\n    ");
                    n = 0;
                }

                fprintf(stream, "%u, ", d);

                if (d < 10)
                    n += 3;
                else if (d < 100)
                    n += 4;
                else
                    n += 5;
            }

            fprintf(stream, "\n};\n");
            fclose(stream);
        }
    }
}

void UpdateProgress(const TaskData& taskData, bool isSucceeded, const char* message)
{
    // IMPORTANT: do not split into several "Printf" calls because multi-threading access to the console can mess up the order
    if (isSucceeded)
    {
        float progress = 100.0f * float(++g_ProcessedTaskCount) / float(g_OriginalTaskCount);

        if (message)
        {
            Printf(YELLOW "[%5.1f%%] %s %s {%s} {%s}\n%s" WHITE,
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
        Printf(RED "[ FAIL ] %s %s {%s} {%s}\n%s" WHITE,
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
            OPT_BOOLEAN(0, "binary", &isBinaryNeeded, "Output native binary files", nullptr, 0, 0),
            OPT_BOOLEAN(0, "header", &isHeaderNeeded, "Output header files", nullptr, 0, 0),
            OPT_BOOLEAN(0, "blob", &isBlobNeeded, "Output shader blob files", nullptr, 0, 0),
            OPT_STRING(0, "compiler", &compiler, "Path to a specific FXC/DXC compiler", nullptr, 0, 0),
        OPT_GROUP("Compiler settings:"),
            OPT_STRING('m', "shaderModel", &shaderModel, "Shader model for DXIL/SPIRV (always SM 5.0 for DXBC)", nullptr, 0, 0),
            OPT_INTEGER('O', "optimization", &optimizationLevel, "Optimization level 0-3 (default = 3, disabled = 0)", nullptr, 0, 0),
            OPT_BOOLEAN(0, "WX", &warningsAreErrors, "Maps to '-WX' DXC/FXC option: warnings are errors", nullptr, 0, 0),
            OPT_BOOLEAN(0, "allResourcesBound", &allResourcesBound, "Maps to '-all_resources_bound' DXC/FXC option: all resources bound", nullptr, 0, 0),
            OPT_BOOLEAN(0, "PDB", &pdb, "Output PDB files in 'out/PDB/' folder", nullptr, 0, 0),
            OPT_BOOLEAN(0, "stripReflection", &stripReflection, "Maps to '-Qstrip_reflect' DXC/FXC option: strip reflection information from a shader binary", nullptr, 0, 0),
            OPT_BOOLEAN(0, "matrixRowMajor", &matrixRowMajor, "Maps to '-Zpr' DXC/FXC option: pack matrices in row-major order", nullptr, 0, 0),
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
            OPT_BOOLEAN(0, "useAPI", &useAPI, "Use FXC (d3dcompiler) or DXC (dxcompiler) API explicitly (Windows only)", nullptr, 0, 0),
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

#ifndef _WIN32
    useAPI = false;
#endif

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

    if (!isBlobNeeded && !isBinaryNeeded && !isHeaderNeeded)
    {
        Printf(RED "ERROR: At least one of 'blob', 'binary' or 'header' must be set!\n");
        return false;
    }

    if (!platformName)
    {
        Printf(RED "ERROR: Platform not specified!\n");
        return false;
    }

    if (!useAPI && !compiler)
    {
        Printf(RED "ERROR: Compiler not specified!\n");
        return false;
    }

    if (!useAPI && !fs::exists(compiler))
    {
        Printf(RED "ERROR: Compiler '%s' does not exist!\n", compiler);
        return false;
    }

    if (strlen(shaderModel) != 3)
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
        OPT_STRING('o', "output", &outputDir, "(Optional) output directory override", nullptr, 0, 0),
        OPT_INTEGER('O', "optimization", &optimizationLevel, "(Optional) optimization level", nullptr, 0, 0),
        OPT_END(),
    };

    static const char* usages[] = {
        "path/to/shader -T profile [-E entry -O{0|1|2|3} -o \"path/to/output\" -D DEF1={0,1} -D DEF2={0,1,2} -D DEF3 ...]",
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

        // Dump outputs
        if (isSucceeded)
            DumpBinaryAndHeader(taskData, (uint8_t*)codeBlob->GetBufferPointer(), codeBlob->GetBufferSize());

        // Update progress
        UpdateProgress(taskData, isSucceeded, errorBlob ? (char*)errorBlob->GetBufferPointer() : nullptr);

        // Terminate if this shader failed and continueOnError is not set
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
            hr = dxcCompiler->Compile(&sourceBuffer, argPointers.data(), (uint32_t)args.size(), pDefaultIncludeHandler, IID_PPV_ARGS(&dxcResult));

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

        // Dump outputs
        if (isSucceeded)
            DumpBinaryAndHeader(taskData, (uint8_t*)codeBlob->GetBufferPointer(), codeBlob->GetBufferSize());

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
            cmd << sourceFile.string().c_str();

            // Output file
            string outputFile = taskData.outputFileWithoutExt + g_OutputExt;
            if (g_Options.isBinaryNeeded || g_Options.isBlobNeeded)
                cmd << " -Fo " << outputFile;
            if (g_Options.isHeaderNeeded)
            {
                fs::path path = taskData.outputFileWithoutExt;
                string name = path.filename().string();
                replace(name.begin(), name.end(), '.', '_');

                cmd << " -Fh " << outputFile << ".h";
                cmd << " -Vn g_" << name << "_" << g_PlatformExts[g_Options.platform] + 1;
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
                cmd << " -I " << dir.string().c_str();

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
                    cmd << " -Fd " << fs::path(outputFile).parent_path().string() << "/" PDB_DIR "/"; // only binary code affects hash
            }
        }

        cmd << " 2>&1";

        if (g_Options.verbose)
            Printf("%s\n", cmd.str().c_str());

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
        Printf(RED "ERROR: Can't open file '%s', included in:\n" WHITE, PathToString(file).c_str());
        for (const fs::path& otherFile : callStack)
            Printf(RED "\t%s\n" WHITE, PathToString(otherFile).c_str());

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
            Printf(RED "ERROR: Can't find include file '%s', included in:\n" WHITE, PathToString(includeName).c_str());
            for (const fs::path& otherFile : callStack)
                Printf(RED "\t%s\n" WHITE, PathToString(otherFile).c_str());

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

bool ProcessConfigLine(uint32_t lineIndex, const string& line, const fs::file_time_type& configWriteTime)
{
    // Tokenize
    string lineCopy = line;
    vector<const char*> tokens;
    TokenizeConfigLine((char*)lineCopy.c_str(), tokens);

    // Parse config line
    ConfigLine configLine;
    if (!configLine.Parse((int32_t)tokens.size(), tokens.data()))
    {
        Printf(RED "%s(%u,0): ERROR: Can't parse config line!\n" WHITE, PathToString(g_Options.configFile).c_str(), lineIndex + 1);

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
    fs::path compiledName = RemoveLeadingDotDots(configLine.source);
    compiledName.replace_extension("");
    if (g_Options.flatten)
        compiledName = compiledName.filename();
    if (strcmp(configLine.entryPoint, "main"))
        compiledName += "_" + string(configLine.entryPoint);

    // Compiled shader permutation name
    fs::path compiledPermutation = compiledName;
    if (!configLine.defines.empty())
    {
        uint32_t permutationHash = HashToUint(hash<string>()(combinedDefines));

        char buf[16];
        snprintf(buf, sizeof(buf), "_%08X", permutationHash);

        compiledPermutation += buf;
    }

    // Output directory
    fs::path destDir = configLine.outputDir ? configLine.outputDir : g_Options.outputDir;

    // Create intermediate output directories
    bool force = g_Options.force;
    fs::path endPath = destDir / compiledName.parent_path();
    if (g_Options.pdb)
        endPath /= PDB_DIR;
    if (endPath.string() != "" && !fs::exists(endPath))
    {
        fs::create_directories(endPath);
        force = true;
    }

    fs::path sourceFile = g_Options.configFile.parent_path() / g_Options.sourceDir / configLine.source;

    // Construct output file name
    fs::path finalOutputFile = destDir;
    if (g_Options.isBlobNeeded)
        finalOutputFile /= compiledName;
    else
        finalOutputFile /= compiledPermutation;
    finalOutputFile += g_OutputExt;
    if (g_Options.isHeaderNeeded)
        finalOutputFile += ".h";

    // If output is available, update modification time heirarchically
    if (!force && fs::exists(finalOutputFile))
    {
        fs::file_time_type fileTime = fs::last_write_time(finalOutputFile);

        fs::file_time_type sourceHierarchyTime;
        list<fs::path> callStack;
        if (!GetHierarchicalUpdateTime(sourceFile, callStack, sourceHierarchyTime))
            return false;

        sourceHierarchyTime = max(sourceHierarchyTime, configWriteTime);
        if (fileTime > sourceHierarchyTime)
            return true;
    }

    // Prepare a task
    fs::path compiledPermutationFileWithoutExt = destDir / compiledPermutation;
    uint32_t optimizationLevel = configLine.optimizationLevel == USE_GLOBAL_OPTIMIZATION_LEVEL ? g_Options.optimizationLevel : configLine.optimizationLevel;
    optimizationLevel = min(optimizationLevel, 3u);

    TaskData& taskData = g_TaskData.emplace_back();
    taskData.source = configLine.source;
    taskData.entryPoint = configLine.entryPoint;
    taskData.profile = configLine.profile;
    taskData.combinedDefines = combinedDefines;
    taskData.outputFileWithoutExt = compiledPermutationFileWithoutExt.string();
    taskData.defines = configLine.defines;
    taskData.optimizationLevel = optimizationLevel;

    // Gather blobs
    if (g_Options.isBlobNeeded && !configLine.defines.empty()) // TODO: should we allow blobs with 1 permutation only?
    {
        BlobEntry entry;
        entry.compiledPermutationFileWithoutExt = compiledPermutationFileWithoutExt;
        entry.permutation = combinedDefines;

        vector<BlobEntry>& entries = g_ShaderBlobs[PathToString(finalOutputFile)];
        entries.push_back(entry);
    }

    return true;
}

bool ExpandPermutations(uint32_t lineIndex, const string& line, const fs::file_time_type& configWriteTime)
{
    size_t opening = line.find('{');
    if (opening == string::npos)
        return ProcessConfigLine(lineIndex, line, configWriteTime);

    size_t closing = line.find('}', opening);
    if (closing == string::npos)
    {
        Printf(RED "%s(%u,0): ERROR: Missing '}'!\n" WHITE, PathToString(g_Options.configFile).c_str(), lineIndex + 1);

        return false;
    }

    size_t current = opening + 1;
    while (true)
    {
        size_t comma = line.find(',', current);
        if (comma == string::npos || comma > closing)
            comma = closing;

        string newConfig = line.substr(0, opening) + line.substr(current, comma - current) + line.substr(closing + 1);
        if (!ExpandPermutations(lineIndex, newConfig, configWriteTime))
            return false;

        current = comma + 1;
        if (comma >= closing)
            break;
    }

    return true;
}

bool CreateBlob(const string& finalOutputFileName, const vector<BlobEntry>& entries)
{
    // Create output file
    FILE* outputStream = fopen(finalOutputFileName.c_str(), "wb");
    if (!outputStream)
    {
        Printf(RED "ERROR: Can't open compiler-generated file '%s'!\n" WHITE, finalOutputFileName.c_str());

        return false;
    }

    // Write "blob" header
    extern const char* g_BlobSignature;
    extern size_t g_BlobSignatureSize;
    fwrite(g_BlobSignature, 1, g_BlobSignatureSize, outputStream);

    bool success = true;

    // Collect individual permutations
    for (const BlobEntry& entry : entries)
    {
        // Open compiled permutation file (source)
        fs::path temp = entry.compiledPermutationFileWithoutExt;
        temp += g_OutputExt;
        string file = PathToString(temp);

        FILE* inputStream = fopen(file.c_str(), "rb");
        if (!inputStream)
        {
            Printf(RED "ERROR: Can't open file source '%s'!\n" WHITE, file.c_str());
            fclose(outputStream);

            return false;
        }

        // Get permutation file size
        uint32_t fileSize = GetFileLength(inputStream);

        // Warn if the file is suspiciously large
        if (fileSize > (64 << 20)) // > 64Mb
            Printf(YELLOW "WARNING: Binary file '%s' is too large!\n" WHITE, file.c_str());

        // Allocate memory foe the whole file
        void* buffer = malloc(fileSize);
        if (buffer)
        {
            // Read the source file
            size_t bytesRead = fread(buffer, 1, fileSize, inputStream);
            if (bytesRead == fileSize)
            {
                ShaderBlobEntry binaryEntry;
                binaryEntry.permutationSize = (uint32_t)entry.permutation.size();
                binaryEntry.dataSize = (uint32_t)fileSize;

                fwrite(&binaryEntry, 1, sizeof(binaryEntry), outputStream);
                fwrite(entry.permutation.data(), 1, entry.permutation.size(), outputStream);
                fwrite(buffer, 1, fileSize, outputStream);
            }
            else
            {
                Printf(YELLOW "ERROR: Failed to read %llu bytes from '%s'!\n", fileSize, file.c_str());
                success = false;
            }

            free(buffer);
        }
        else if (fileSize)
        {
            Printf(YELLOW "ERROR: Can't allocate %u bytes!\n" WHITE, fileSize);
            success = false;
        }
        else
        {
            Printf(YELLOW "WARNING: Binary file '%s' is empty!\n" WHITE, file.c_str());
            success = false;
        }

        fclose(inputStream);

        if (!g_Options.isBinaryNeeded)
            fs::remove(file);
    }

    fclose(outputStream);

    return success;
}

void SignalHandler(int32_t sig)
{
    UNUSED(sig);

    g_Terminate = true;

    Printf(YELLOW "Aborting...\n");
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

        if (g_Options.verbose)
            Printf("%s\n", envBuf);
    }

#ifdef _WIN32
    // Setup a directory where to look for "dxcompiler" first
    if (g_Options.compiler)
    {
        fs::path compilerDir = fs::path(g_Options.compiler).parent_path();
        if (g_Options.platform != DXBC && compilerDir != "")
            SetDllDirectoryA(compilerDir.string().c_str());
    }
#endif

    { // Gather shader permutations
        fs::file_time_type configWriteTime = fs::last_write_time(g_Options.configFile);
        configWriteTime = max(configWriteTime, fs::last_write_time(self));

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
                    Printf(RED "%s(%u,0): ERROR: Unexpected '#endif'!\n" WHITE, PathToString(g_Options.configFile).c_str(), lineIndex + 1);
                else
                    blocks.pop_back();
            }
            else if (line.find("#else") != string::npos)
            {
                if (blocks.size() < 2)
                    Printf(RED "%s(%u,0): ERROR: Unexpected '#else'!\n" WHITE, PathToString(g_Options.configFile).c_str(), lineIndex + 1);
                else if (blocks[blocks.size() - 2])
                    blocks.back() = !blocks.back();
            }
            else if (blocks.back())
            {
                if (!ExpandPermutations(lineIndex, line, configWriteTime))
                    return 1;
            }
        }
    }

    // Process tasks
    if (!g_TaskData.empty())
    {
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
        if (g_Options.isBlobNeeded && g_FailedTaskCount == 0)
        {
            for (const auto& it : g_ShaderBlobs)
            {
                if (!CreateBlob(it.first, it.second))
                    return 1;
            }
        }

        // Report failed tasks
        if (g_FailedTaskCount)
            Printf(YELLOW "WARNING: %u task(s) failed to complete!\n" WHITE, g_FailedTaskCount.load());
        else
            Printf("%d task(s) completed successfully.\n", g_OriginalTaskCount);

        uint64_t end = Timer_GetTicks();
        Printf("Elapsed time %.2f ms\n\n", Timer_ConvertTicksToMilliseconds(end - start));
    }
    else
        Printf("All %s shaders are up to date.\n", g_Options.platformName);

    return (g_Terminate || g_FailedTaskCount) ? 1 : 0;
}
