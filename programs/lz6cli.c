/*
  LZ6cli - LZ6 Command Line Interface
  Copyright (C) Yann Collet 2011-2015

  GPL v2 License

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

  You can contact the author at :
  - LZ6 source repository : https://github.com/inikep/lz6
  - LZ6 public forum : https://groups.google.com/forum/#!forum/lz6c
*/
/*
  Note : this is stand-alone program.
  It is not part of LZ6 compression library, it is a user program of the LZ6 library.
  The license of LZ6 library is BSD.
  The license of xxHash library is BSD.
  The license of this compression CLI program is GPLv2.
*/


/**************************************
*  Compiler Options
***************************************/
/* Disable some Visual warning messages */
#ifdef _MSC_VER
#  define _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_DEPRECATE     /* VS2005 */
#  pragma warning(disable : 4127)      /* disable: C4127: conditional expression is constant */
#endif

/****************************
*  Includes
*****************************/
#include <stdio.h>    /* fprintf, getchar */
#include <stdlib.h>   /* exit, calloc, free */
#include <string.h>   /* strcmp, strlen */
#include "bench.h"    /* BMK_benchFile, BMK_SetNbIterations, BMK_SetBlocksize, BMK_SetPause */
#include "lz6io.h"    /* LZ6IO_compressFilename, LZ6IO_decompressFilename, LZ6IO_compressMultipleFilenames */
#include "lz6.h"      // LZ6_VERSION
#include "entropy/lz6seq.h"   /* LZ6_compress_seq, LZ6_decompress_seq (--seq mode) */

/*-************************************
*  OS-specific Includes
**************************************/
#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(_WIN32) || defined(__CYGWIN__)
#  include <io.h>       /* _isatty */
#else
#  include <unistd.h>   /* isatty */
#endif 
#if defined(_POSIX_C_SOURCE) || defined(_XOPEN_SOURCE) || defined(_POSIX_SOURCE)
#  define IS_CONSOLE(stdStream) isatty(fileno(stdStream))
#else
#  define IS_CONSOLE(stdStream) 0
#endif


/*****************************
*  Constants
******************************/
#define COMPRESSOR_NAME "LZ6 command line interface"
#define AUTHOR "Y.Collet & P.Skibinski"
#define WELCOME_MESSAGE "%s %i-bit %s by %s (%s)\n", COMPRESSOR_NAME, (int)(sizeof(void*)*8), LZ6_VERSION, AUTHOR, __DATE__
#define LZ6_EXTENSION ".lz6"
#define LZ6CAT "lz6cat"
#define UNLZ6 "unlz6"

#define LZ6_BLOCKSIZEID_DEFAULT 5


/**************************************
*  Macros
***************************************/
#define DISPLAY(...)           fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...)   if (displayLevel>=l) { DISPLAY(__VA_ARGS__); }
static unsigned displayLevel = 2;   /* 0 : no display ; 1: errors only ; 2 : downgradable normal ; 3 : non-downgradable normal; 4 : + information */


/**************************************
*  Local Variables
***************************************/
static char* programName;


/**************************************
*  Exceptions
***************************************/
#define DEBUG 0
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "Error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, "\n");                                                \
    exit(error);                                                          \
}


/**************************************
*  Version modifiers
***************************************/
#define EXTENDED_ARGUMENTS
#define EXTENDED_HELP
#define EXTENDED_FORMAT
#define DEFAULT_COMPRESSOR   LZ6IO_compressFilename
#define DEFAULT_DECOMPRESSOR LZ6IO_decompressFilename


/*****************************
*  Functions
*****************************/
static int usage(void)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [arg] [input] [output]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "input   : a filename\n");
    DISPLAY( "          with no FILE, or when FILE is - or %s, read standard input\n", stdinmark);
    DISPLAY( "Arguments :\n");
    DISPLAY( " -0       : Fast compression (default) \n");
    DISPLAY( " -1...-%d : High compression; higher number == more compression but slower\n", LZ6HC_MAX_CLEVEL);
    DISPLAY( " -d       : decompression (default for %s extension)\n", LZ6_EXTENSION);
    DISPLAY( " -z       : force compression\n");
    DISPLAY( " -f       : overwrite output without prompting \n");
    DISPLAY( " -h/-H    : display help/long help and exit\n");
    return 0;
}

static int usage_advanced(void)
{
    DISPLAY(WELCOME_MESSAGE);
    usage();
    DISPLAY( "\n");
    DISPLAY( "Advanced arguments :\n");
    DISPLAY( " -V     : display Version number and exit\n");
    DISPLAY( " -v     : verbose mode\n");
    DISPLAY( " -q     : suppress warnings; specify twice to suppress errors too\n");
    DISPLAY( " -c     : force write to standard output, even if it is the console\n");
    DISPLAY( " -t     : test compressed file integrity\n");
    DISPLAY( " -m     : multiple input files (implies automatic output filenames)\n");
    DISPLAY( " -B#    : Block size [1-7] = 64KB, 256KB, 1MB, 4MB, 16MB, 64MB, 256MB (default : 5 = 16MB)\n");
  //  DISPLAY( " -BD    : Block dependency (improve compression ratio)\n");
    /* DISPLAY( " -BX    : enable block checksum (default:disabled)\n");   *//* Option currently inactive */
    DISPLAY( "--no-frame-crc : disable stream checksum (default:enabled)\n");
    DISPLAY( "--content-size : compressed frame includes original size (default:not present)\n");
    DISPLAY( "--[no-]sparse  : sparse mode (default:enabled on file, disabled on stdout)\n");
    DISPLAY( "--seq          : use the lz6seq entropy codec (LZ + FSE/rANS, non-standard format)\n");
    DISPLAY( "Benchmark arguments :\n");
    DISPLAY( " -b     : benchmark file(s)\n");
    DISPLAY( " -i#    : iteration loops [1-9](default : 3), benchmark mode only\n");
    EXTENDED_HELP;
    return 0;
}

static int usage_longhelp(void)
{
    usage_advanced();
    DISPLAY( "\n");
    DISPLAY( "****************************\n");
    DISPLAY( "***** Advanced comment *****\n");
    DISPLAY( "****************************\n");
    DISPLAY( "\n");
    DISPLAY( "Which values can [output] have ? \n");
    DISPLAY( "---------------------------------\n");
    DISPLAY( "[output] : a filename \n");
    DISPLAY( "          '%s', or '-' for standard output (pipe mode)\n", stdoutmark);
    DISPLAY( "          '%s' to discard output (test mode) \n", NULL_OUTPUT);
    DISPLAY( "[output] can be left empty. In this case, it receives the following value :\n");
    DISPLAY( "          - if stdout is not the console, then [output] = stdout \n");
    DISPLAY( "          - if stdout is console : \n");
    DISPLAY( "               + for compression, output to filename%s \n", LZ6_EXTENSION);
    DISPLAY( "               + for decompression, output to filename without '%s'\n", LZ6_EXTENSION);
    DISPLAY( "                    > if input filename has no '%s' extension : error \n", LZ6_EXTENSION);
    DISPLAY( "\n");
    DISPLAY( "Compression levels : \n");
    DISPLAY( "---------------------\n");
    DISPLAY( "-0 => Fast compression\n");
    DISPLAY( "-1 ... -%d => High compression; higher number == more compression but slower\n", LZ6HC_MAX_CLEVEL);
    DISPLAY( "\n");
    DISPLAY( "stdin, stdout and the console : \n");
    DISPLAY( "--------------------------------\n");
    DISPLAY( "To protect the console from binary flooding (bad argument mistake)\n");
    DISPLAY( "%s will refuse to read from console, or write to console \n", programName);
    DISPLAY( "except if '-c' command is specified, to force output to console \n");
    DISPLAY( "\n");
    DISPLAY( "Simple example :\n");
    DISPLAY( "----------------\n");
    DISPLAY( "1 : compress 'filename' fast, using default output name 'filename.lz6'\n");
    DISPLAY( "          %s filename\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Short arguments can be aggregated. For example :\n");
    DISPLAY( "----------------------------------\n");
    DISPLAY( "2 : compress 'filename' in high compression mode, overwrite output if exists\n");
    DISPLAY( "          %s -9 -f filename \n", programName);
    DISPLAY( "    is equivalent to :\n");
    DISPLAY( "          %s -9f filename \n", programName);
    DISPLAY( "\n");
    DISPLAY( "%s can be used in 'pure pipe mode'. For example :\n", programName);
    DISPLAY( "-------------------------------------\n");
    DISPLAY( "3 : compress data stream from 'generator', send result to 'consumer'\n");
    DISPLAY( "          generator | %s | consumer \n", programName);
    return 0;
}

static int badusage(void)
{
    DISPLAYLEVEL(1, "Incorrect parameters\n");
    if (displayLevel >= 1) usage();
    exit(1);
}


static void waitEnter(void)
{
    DISPLAY("Press enter to continue...\n");
    (void)getchar();
}


/* --seq mode: lz6seq entropy codec, simple file-in/file-out (or stdin/stdout).
   Returns 0 on success, non-zero on error. */
static int seq_mode_main(const char* input_filename, const char* output_filename,
                         int decode, int cLevel, int displayLevel)
{
    FILE* fin = stdin;
    FILE* fout = stdout;
    int close_in = 0, close_out = 0;
    long sz;
    unsigned char* in;
    unsigned char* out;
    size_t outsz;
    int rc = 1;

    if (input_filename && strcmp(input_filename, stdinmark) != 0) {
        fin = fopen(input_filename, "rb");
        if (!fin) { DISPLAYLEVEL(1, "lz6seq: cannot open %s\n", input_filename); return 1; }
        close_in = 1;
    }
    if (output_filename && strcmp(output_filename, stdoutmark) != 0) {
        fout = fopen(output_filename, "wb");
        if (!fout) { DISPLAYLEVEL(1, "lz6seq: cannot open %s\n", output_filename); if (close_in) fclose(fin); return 1; }
        close_out = 1;
    }

    /* read all input */
    if (fseek(fin, 0, SEEK_END) == 0) {
        sz = ftell(fin);
        fseek(fin, 0, SEEK_SET);
    } else {
        /* stream (stdin): read until EOF */
        size_t cap = 1 << 20, len = 0;
        in = (unsigned char*)malloc(cap);
        if (!in) goto cleanup;
        while (!feof(fin)) {
            if (len == cap) { cap *= 2; unsigned char* ni = (unsigned char*)realloc(in, cap); if (!ni) { free(in); goto cleanup; } in = ni; }
            len += fread(in + len, 1, cap - len, fin);
        }
        sz = (long)len;
        goto have_input;
    }
    in = (unsigned char*)malloc(sz ? (size_t)sz : 1);
    if (!in) goto cleanup;
    if (sz && fread(in, 1, (size_t)sz, fin) != (size_t)sz) { DISPLAYLEVEL(1, "lz6seq: read error\n"); free(in); goto cleanup; }
have_input:

    if (!decode) {
        size_t cap = (size_t)sz + (size_t)sz / 2 + 65536;
        out = (unsigned char*)malloc(cap);
        if (!out) { free(in); goto cleanup; }
        outsz = LZ6_compress_seq((const char*)in, (size_t)sz, (char*)out, cap,
                                 cLevel > 0 ? cLevel : 1);
        if (!outsz) { DISPLAYLEVEL(1, "lz6seq: compression failed\n"); free(in); free(out); goto cleanup; }
        DISPLAYLEVEL(2, "lz6seq: %ld -> %zu bytes (%.2f%%)\n", sz, outsz, 100.0 * outsz / sz);
    } else {
        /* decode: the original size is stored as isize in the seq stream
         * (bytes 1..4, LE). Allocate that + slack so large outputs fit. */
        size_t orig = 0;
        if (sz >= 5) {
            orig = (size_t)(unsigned char)in[1]
                 | ((size_t)(unsigned char)in[2] << 8)
                 | ((size_t)(unsigned char)in[3] << 16)
                 | ((size_t)(unsigned char)in[4] << 24);
        }
        size_t cap = orig ? orig + 65536 : (size_t)sz * 4 + (1 << 20);
        out = (unsigned char*)malloc(cap);
        if (!out) { free(in); goto cleanup; }
        outsz = LZ6_decompress_seq((const char*)in, (size_t)sz, (char*)out, cap);
        if (!outsz) { DISPLAYLEVEL(1, "lz6seq: decompression failed\n"); free(in); free(out); goto cleanup; }
    }

    if (fwrite(out, 1, outsz, fout) != outsz) { DISPLAYLEVEL(1, "lz6seq: write error\n"); free(in); free(out); goto cleanup; }
    free(in);
    free(out);
    rc = 0;

cleanup:
    if (close_in) fclose(fin);
    if (close_out) fclose(fout);
    return rc;
}


int main(int argc, char** argv)
{
    int i,
        cLevel=0,
        decode=0,
        bench=0,
        forceStdout=0,
        forceCompress=0,
        main_pause=0,
        multiple_inputs=0,
        seqMode=0,
        operationResult=0,
        blockSizeIdUserSet=0;
    const char* input_filename=0;
    const char* output_filename=0;
    char* dynNameSpace=0;
    const char** inFileNames = NULL;
    unsigned ifnIdx=0;
    char nullOutput[] = NULL_OUTPUT;
    char extension[] = LZ6_EXTENSION;
    int  blockSize;

    /* Init */
    programName = argv[0];
    LZ6IO_setOverwrite(0);
    blockSize = LZ6IO_setBlockSizeID(LZ6_BLOCKSIZEID_DEFAULT);

    /* lz6cat predefined behavior */
    if (!strcmp(programName, LZ6CAT)) { decode=1; forceStdout=1; output_filename=stdoutmark; displayLevel=1; }
    if (!strcmp(programName, UNLZ6)) { decode=1; }

    /* command switches */
    for(i=1; i<argc; i++)
    {
        char* argument = argv[i];

        if(!argument) continue;   /* Protection if argument empty */

        /* long commands (--long-word) */
        if (!strcmp(argument,  "--compress")) { forceCompress = 1; continue; }
        if ((!strcmp(argument, "--decompress"))
         || (!strcmp(argument, "--uncompress"))) { decode = 1; continue; }
        if (!strcmp(argument,  "--multiple")) { multiple_inputs = 1; if (inFileNames==NULL) inFileNames = (const char**)malloc(argc * sizeof(char*)); continue; }
        if (!strcmp(argument,  "--test")) { decode = 1; LZ6IO_setOverwrite(1); output_filename=nulmark; continue; }
        if (!strcmp(argument,  "--force")) { LZ6IO_setOverwrite(1); continue; }
        if (!strcmp(argument,  "--no-force")) { LZ6IO_setOverwrite(0); continue; }
        if ((!strcmp(argument, "--stdout"))
         || (!strcmp(argument, "--to-stdout"))) { forceStdout=1; output_filename=stdoutmark; displayLevel=1; continue; }
        if (!strcmp(argument,  "--frame-crc")) { LZ6IO_setStreamChecksumMode(1); continue; }
        if (!strcmp(argument,  "--no-frame-crc")) { LZ6IO_setStreamChecksumMode(0); continue; }
        if (!strcmp(argument,  "--content-size")) { LZ6IO_setContentSize(1); continue; }
        if (!strcmp(argument,  "--no-content-size")) { LZ6IO_setContentSize(0); continue; }
        if (!strcmp(argument,  "--sparse")) { LZ6IO_setSparseFile(2); continue; }
        if (!strcmp(argument,  "--no-sparse")) { LZ6IO_setSparseFile(0); continue; }
        if (!strcmp(argument,  "--verbose")) { displayLevel=4; continue; }
        if (!strcmp(argument,  "--quiet")) { if (displayLevel) displayLevel--; continue; }
        if (!strcmp(argument,  "--version")) { DISPLAY(WELCOME_MESSAGE); return 0; }
        if (!strcmp(argument,  "--keep")) { continue; }   /* keep source file (default anyway; just for xz/lzma compatibility) */
        if (!strcmp(argument,  "--seq")) { seqMode = 1; continue; }   /* use the lz6seq entropy codec */


        /* Short commands (note : aggregated short commands are allowed) */
        if (argument[0]=='-')
        {
            /* '-' means stdin/stdout */
            if (argument[1]==0)
            {
                if (!input_filename) input_filename=stdinmark;
                else output_filename=stdoutmark;
            }

            while (argument[1]!=0)
            {
                argument ++;

                if ((*argument>='0') && (*argument<='9'))
                {
                    cLevel = 0;
                    while ((*argument >= '0') && (*argument <= '9'))
                    {
                        cLevel *= 10;
                        cLevel += *argument - '0';
                        argument++;
                    }
                    argument--;
                    continue;
                }

                switch(argument[0])
                {
                    /* Display help */
                case 'V': DISPLAY(WELCOME_MESSAGE); goto _cleanup;   /* Version */
                case 'h': usage_advanced(); goto _cleanup;
                case 'H': usage_longhelp(); goto _cleanup;

                    /* Compression (default) */
                case 'z': forceCompress = 1; break;

                    /* Decoding */
                case 'd': decode=1; break;

                    /* Force stdout, even if stdout==console */
                case 'c': forceStdout=1; output_filename=stdoutmark; displayLevel=1; break;

                    /* Test integrity */
                case 't': decode=1; LZ6IO_setOverwrite(1); output_filename=nulmark; break;

                    /* Overwrite */
                case 'f': LZ6IO_setOverwrite(1); break;

                    /* Verbose mode */
                case 'v': displayLevel=4; break;

                    /* Quiet mode */
                case 'q': if (displayLevel) displayLevel--; break;

                    /* keep source file (default anyway, so useless) (for xz/lzma compatibility) */
                case 'k': break;

                    /* Modify Block Properties */
                case 'B':
                    while (argument[1]!=0)
                    {
                        int exitBlockProperties=0;
                        switch(argument[1])
                        {
                  //      case 'D': LZ6IO_setBlockMode(LZ6IO_blockLinked); argument++; break;
                        case 'X': LZ6IO_setBlockChecksumMode(1); argument ++; break;   /* currently disabled */
                        default : 
                            {
                                int B = atoi(argument+1);
                                if (B >= 1 && B <= 7)
                                {
                                    blockSize = LZ6IO_setBlockSizeID(B);
                                //    printf("LZ6IO_setBlockSizeID %d %d\n", B, blockSize);
                                    BMK_setBlocksize(blockSize);
                                    blockSizeIdUserSet = 1;   /* explicit -B overrides size-adaptive default below */
                                    argument++;
                                }
                                else                                                            
                                    exitBlockProperties=1;
                                break;
                            }
                        }
                        if (exitBlockProperties) break;
                    }
                    break;

                    /* Benchmark */
                case 'b': bench=1; multiple_inputs=1;
                    if (inFileNames == NULL)
                        inFileNames = (const char**) malloc(argc * sizeof(char*));
                    break;

                    /* Treat non-option args as input files.  See https://code.google.com/p/lz6/issues/detail?id=151 */
                case 'm': multiple_inputs=1;
                    if (inFileNames == NULL)
                        inFileNames = (const char**) malloc(argc * sizeof(char*));
                    break;

                    /* Modify Nb Iterations (benchmark only) */
                case 'i':
                    {
                        unsigned iters = 0;
                        while ((argument[1] >='0') && (argument[1] <='9'))
                        {
                            iters *= 10;
                            iters += argument[1] - '0';
                            argument++;
                        }
                        BMK_setNbIterations(iters);
                    }
                    break;

                    /* Pause at the end (hidden option) */
                case 'p': main_pause=1; BMK_setPause(); break;

                    /* Specific commands for customized versions */
                EXTENDED_ARGUMENTS;

                    /* Unrecognised command */
                default : badusage();
                }
            }
            continue;
        }

        /* Store in *inFileNames[] if -m is used. */
        if (multiple_inputs) { inFileNames[ifnIdx++]=argument; continue; }

        /* Store first non-option arg in input_filename to preserve original cli logic. */
        if (!input_filename) { input_filename=argument; continue; }

        /* Second non-option arg in output_filename to preserve original cli logic. */
        if (!output_filename)
        {
            output_filename=argument;
            if (!strcmp (output_filename, nullOutput)) output_filename = nulmark;
            continue;
        }

        /* 3rd non-option arg should not exist */
        DISPLAYLEVEL(1, "Warning : %s won't be used ! Do you want multiple input files (-m) ? \n", argument);
    }

    DISPLAYLEVEL(3, WELCOME_MESSAGE);

    /* No input filename ==> use stdin */
    if (multiple_inputs) input_filename = inFileNames[0], output_filename = (const char*)(inFileNames[0]);
    if(!input_filename) { input_filename=stdinmark; }

    /* Size-adaptive block-size default: only when the user did not pass -B
       explicitly, and this is a single-file compression of a real
       (non-stdin) input. Blocks compress independently (no cross-block
       back-references; see LZ6IO's default block-independence mode), so an
       input spanning multiple blocks pays a per-boundary ratio cost. Once
       the input no longer fits in one default-size (B5, 16MB) block, bump
       to B6 (64MB): measured ~0.9-1% smaller output on multi-block inputs
       for a memory/time cost that is only paid on inputs large enough to
       matter. Deliberately capped at B6 -- B7 was not measured for this
       policy. Multi-file (-m) and benchmark runs are left at the fixed
       default since one blockSizeID is shared across all their inputs. */
    if (!decode && !multiple_inputs && !blockSizeIdUserSet
        && input_filename && strcmp(input_filename, stdinmark) != 0)
    {
        unsigned long long const knownSize = LZ6IO_GetFileSize(input_filename);
        if (knownSize > (unsigned long long)blockSize)
            blockSize = LZ6IO_setBlockSizeID(6);
    }

    if (!decode) DISPLAYLEVEL(4, "Blocks size : %i KB\n", blockSize>>10);

    /* Check if input is defined as console; trigger an error in this case */
    if (!strcmp(input_filename, stdinmark) && IS_CONSOLE(stdin) )
    {
        DISPLAYLEVEL(1, "refusing to read from a console\n");
        exit(1);
    }

    /* Check if benchmark is selected */
    if (bench)
    {
        int bmkResult = BMK_benchFiles(inFileNames, ifnIdx, cLevel);
        free((void*)inFileNames);
        return bmkResult;
    }

    /* No output filename ==> try to select one automatically (when possible) */
    while (!output_filename)
    {
        if (!IS_CONSOLE(stdout)) { output_filename=stdoutmark; break; }   /* Default to stdout whenever possible (i.e. not a console) */
        if ((!decode) && !(forceCompress))   /* auto-determine compression or decompression, based on file extension */
        {
            size_t l = strlen(input_filename);
            if (!strcmp(input_filename+(l-4), LZ6_EXTENSION)) decode=1;
        }
        if (!decode)   /* compression to file */
        {
            size_t l = strlen(input_filename);
            dynNameSpace = (char*)calloc(1,l+5);
			if (dynNameSpace==NULL) exit(1);
            strcpy(dynNameSpace, input_filename);
            strcat(dynNameSpace, LZ6_EXTENSION);
            output_filename = dynNameSpace;
            DISPLAYLEVEL(2, "Compressed filename will be : %s \n", output_filename);
            break;
        }
        /* decompression to file (automatic name will work only if input filename has correct format extension) */
        {
            size_t outl;
            size_t inl = strlen(input_filename);
            dynNameSpace = (char*)calloc(1,inl+1);
            strcpy(dynNameSpace, input_filename);
            outl = inl;
            if (inl>4)
                while ((outl >= inl-4) && (input_filename[outl] ==  extension[outl-inl+4])) dynNameSpace[outl--]=0;
            if (outl != inl-5) { DISPLAYLEVEL(1, "Cannot determine an output filename\n"); badusage(); }
            output_filename = dynNameSpace;
            DISPLAYLEVEL(2, "Decoding file %s \n", output_filename);
        }
    }

    /* Check if output is defined as console; trigger an error in this case */
    if (!strcmp(output_filename,stdoutmark) && !forceStdout && IS_CONSOLE(stdout))
    {
        DISPLAYLEVEL(1, "refusing to write to console without -c\n");
        exit(1);
    }

    /* Downgrade notification level in pure pipe mode (stdin + stdout) and multiple file mode */
    if (!strcmp(input_filename, stdinmark) && !strcmp(output_filename,stdoutmark) && (displayLevel==2)) displayLevel=1;
    if ((multiple_inputs) && (displayLevel==2)) displayLevel=1;


    /* IO Stream/File */
    LZ6IO_setNotificationLevel(displayLevel);
    if (seqMode)
    {
        /* lz6seq entropy codec path: simple file-in/file-out (or stdin/stdout). */
        operationResult = seq_mode_main(input_filename, output_filename, decode, cLevel, displayLevel);
    }
    else if (decode)
    {
      if (multiple_inputs)
        operationResult = LZ6IO_decompressMultipleFilenames(inFileNames, ifnIdx, LZ6_EXTENSION);
      else
        operationResult = DEFAULT_DECOMPRESSOR(input_filename, output_filename);
    }
    else
    {
      /* compression is default action */
      {
        if (multiple_inputs)
          operationResult = LZ6IO_compressMultipleFilenames(inFileNames, ifnIdx, LZ6_EXTENSION, cLevel);
        else
          operationResult = DEFAULT_COMPRESSOR(input_filename, output_filename, cLevel);
      }
    }

_cleanup:
    if (main_pause) waitEnter();
    free(dynNameSpace);
    free((void*)inFileNames);
    return operationResult;
}
