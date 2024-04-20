/*********************************************************************************
 * MIT License                                                                   *
 *                                                                               *
 * Copyright (c) 2024 Chenxi Zhou <chnx.zhou@gmail.com>                          *
 *                                                                               *
 * Permission is hereby granted, free of charge, to any person obtaining a copy  *
 * of this software and associated documentation files (the "Software"), to deal *
 * in the Software without restriction, including without limitation the rights  *
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell     *
 * copies of the Software, and to permit persons to whom the Software is         *
 * furnished to do so, subject to the following conditions:                      *
 *                                                                               *
 * The above copyright notice and this permission notice shall be included in    *
 * all copies or substantial portions of the Software.                           *
 *                                                                               *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR    *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,      *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE   *
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER        *
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, *
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE *
 * SOFTWARE.                                                                     *
 *********************************************************************************/

/********************************** Revision History *****************************
 *                                                                               *
 * 16/04/24 - Chenxi Zhou: Created                                               *
 *                                                                               *
 *********************************************************************************/

#ifdef __linux__
#define _GNU_SOURCE  // needed for vasprintf() on Linux
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <zlib.h>

#include "DB.h"
#include "align.h"
#include "alncode.h"

#undef DEBUG_DICT
#undef DEBUG_READ_1ALN
#undef DEBUG_PARSE_SEQ

static char *Usage[] = { "[-dpSL] [-T<int(1)>] [-l<int(50)>] [-i<float(0.7)>] [-f<int(11)>]",
                         "[-x<target>] [-y<target>] [-H<int(600)>] [-W<int>] [-o<output:path>[.pdf]]",
                         "<alignment:path[.1aln]>"
                       };

static int    MINALEN  = 50;
static int    IMGWIDTH = 0;
static int    IMGHEIGH = 0;
static int    FONTSIZE = 11;
static int    NOLABEL  = 0;
static int    PRINTSID = 0;
static int    TRYADIAG = 0;
static int    PAFINPUT = 0;
static double MINAIDNT = 0.7;
static int    NTHREADS = 1;
static char  *OUTPDF   = NULL;
static char  *OUTEPS   = NULL;

#define NUM_SYMBOL  '#'
#define FIL_SYMBOL  '@'
#define SEP_SYMBOL  ','

#define MAX_XY_LEN 10000
#define MIN_XY_LEN 50

typedef struct
  { int aread, bread;
    int abpos, bbpos;
    int aepos, bepos;
  } Segment;

static Segment  *segments = NULL;
static int64     nSegment = 0;
static int      *ASEQ, *BSEQ; // -1 for excluded sequences

typedef struct
  { int64    beg;
    int64    end;
    OneFile *in;
    Segment *segs;
    int64    nseg;
  } Packet;

/***********************************************************************************
 *
 *    DICTIONARY: string hash map
 *    adapted from Richard's implementation
 *
 **********************************************************************************/

typedef struct
  { char **names;
    uint64 *table;
    uint64 max;         /* current number of entries */
    uint64 dim;
    uint64 size;        /* 2^dim = size of tables */
    uint64 new;         /* communication between dictFind() and dictAdd() */
  } DICT;

static int    ISTWO;         // If two DB are different
static DICT  *Adict, *Bdict; // Sequence dictionary
static int   *ALEN, *BLEN;   // Map sequence to its length
//  for 1aln contig to scaffold mapping
static int   *AMAP, *BMAP;   // Contig to scaffold map
static int   *AOFF, *BOFF;   // Contig offset map

#define dictMax(dict)  ((dict)->max)

static void *remap(void *old, uint64 oldSize, uint64 newSize)
{ void *new = Malloc(newSize, "Allocating name array");
  memset(new, 0, newSize);
  memcpy(new, old, oldSize);
  free(old);
  return new;
}

static uint64 hashString(char *cp, uint64 n, int isDiff)
{ uint64 i;
  uint64 j, x = 0;
  uint64 rotate = isDiff ? 21 : 13;
  uint64 leftover = 8 * sizeof(uint64) - rotate;

  while (*cp)
    x = (*cp++) ^ ((x >> leftover) | (x << rotate));

  for (j = x, i = n; i < sizeof(uint64); i += n)
    j ^= (x >> i);
  j &= (1 << n) - 1;

  if (isDiff)
    j |= 1;

  return j;
}

DICT *dictCreate(uint64 size)
{ DICT *dict = (DICT *) Malloc (sizeof(DICT), "Allocating dictionary");
  memset(dict, 0, sizeof(DICT));
  for (dict->dim = 10, dict->size = 1024; dict->size < size; dict->dim += 1, dict->size *= 2);
  dict->table = (uint64 *) Malloc(sizeof(uint64) * dict->size, "Allocating dictionary table");
  memset(dict->table, 0, sizeof(uint64) * dict->size);
  dict->names = (char **) Malloc (sizeof(char *) * dict->size / 2, "Allocating dictionary names");
  memset(dict->names, 0, sizeof(char *) * dict->size / 2);
  return dict;
}

void dictDestroy(DICT *dict)
{ uint64 i;
  for (i = 1; i <= dict->max; ++i) free(dict->names[i]);
  free(dict->names);
  free(dict->table);
  free(dict);
}

int dictFind(DICT *dict, char *s, uint64 *index)
{ uint64 i, x, d;
  
  if (!dict || !s) return 0;

  x = hashString (s, dict->dim, 0);
  if (!(i = dict->table[x]))
    { dict->new = x;
      return 0;
    }
  else if (!strcmp (s, dict->names[i]))
    { if (index)
        *index = i-1;
      return 1;
    }
  else
    { d = hashString (s, dict->dim, 1);
      while (1)
        { x = (x + d) & ((1 << dict->dim) - 1);
          if (!(i = dict->table[x]))
            { dict->new = x;
              return 0;
            }
          else if (!strcmp (s, dict->names[i]))
          {
            if (index)
              *index = i-1;
            return 1;
          }
        }
    }
  return 0;
}

int dictAdd(DICT *dict, char *s, uint64 *index)
{ uint64 i, x;

  if (dictFind(dict, s, index)) return 0;

  i = ++dict->max;
  dict->table[dict->new] = i;
  dict->names[i] = (char*) Malloc(strlen(s) + 1, "Allocating name space");
  strcpy(dict->names[i], s);
  if (index) *index = i-1;

  if (dict->max > 0.3 * dict->size) /* double table size and remap */
    { uint64 *newTable;
      dict->dim += 1;
      dict->size *= 2;
      dict->names = (char **) remap(dict->names, sizeof(char *) * (dict->max + 1), sizeof(char *) * (dict->size / 2));
      newTable = (uint64 *) Malloc(sizeof(uint64) * dict->size, "Allocating new table");
      memset(newTable, 0, sizeof(uint64) * dict->size);
      for (i = 1; i <= dict->max; i++)
        { s = dict->names[i];
          x = hashString (s, dict->dim, 0);
         if (!newTable[x])
           newTable[x] = i;
         else
           { uint64 d = hashString (s, dict->dim, 1);
             while (1)
               { x = (x + d) & ((1 << dict->dim) - 1);
                 if (!newTable[x])
                   { newTable[x] = i;
                     break;
                   }
               }
           }
        }
      free(dict->table);
      dict->table = newTable;
    }
  
  return 1;
}

char *dictName(DICT *dict, uint64 i)
{ return dict->names[i+1]; }

/**********************************************************************************/

/***********************************************************************************
 *
 *    UTILITIES: exit function
 *    adapted from Richard's utilities
 *
 **********************************************************************************/
static void die(char *format, ...)
{ va_list args;

  va_start (args, format);
  fprintf (stderr, "FATAL ERROR: ");
  vfprintf (stderr, format, args);
  fprintf (stderr, "\n");
  va_end (args);
  exit (-1);
}

void warn(char *format, ...)
{ va_list args ;

  va_start (args, format) ;
  fprintf (stderr, "WARNING: ") ;
  vfprintf (stderr, format, args) ;
  fprintf (stderr, "\n") ;
  va_end (args) ;
}
/**********************************************************************************/

int run_system_cmd(char *cmd, int retry)
{ int exit_code = system(cmd);
  --retry;
  if ((exit_code != -1 && !WEXITSTATUS(exit_code)) || !retry)
    return exit_code;
  return run_system_cmd(cmd, retry);
}

int check_executable(char *exe)
{ char cmd[4096];
  sprintf(cmd, "%s -h >/dev/null 2>&1", exe);
  
  int exit_code = run_system_cmd(cmd, 1);
  if (exit_code == -1 || WEXITSTATUS(exit_code))
    { warn("%s: executable %s is not available",Prog_Name,exe);
      return 0;
    }
  return 1;
}

void makeSeqDICTFromDB(char *db1_name, char *db2_name)
{ DAZZ_DB   _db1, *db1 = &_db1;
  DAZZ_DB   _db2, *db2 = &_db2;

  //  Open DB or DB pair

  { int   s, r, gdb;

    ISTWO  = 0;
    gdb    = Open_DB(db1_name,db1);
    if (gdb < 0)
      die("%s: Could not open DB file: %s",Prog_Name,db1_name);

    if (db2_name != NULL)
      { gdb = Open_DB(db2_name,db2);
        if (gdb < 0)
          die("%s: Could not open DB file: %s",Prog_Name,db2_name);
        ISTWO = 1;
      }
    else
      db2  = db1;

    //  Build contig to scaffold maps in global vars

    if (ISTWO)
    {
      AMAP = (int *) Malloc(sizeof(int)*2*(db1->treads+db2->treads),"Allocating scaffold map");
      ALEN = (int *) Malloc(sizeof(int)*(db1->nreads+db2->nreads),"Allocating scaffold length map");
    }
    else
    {
      AMAP = (int *) Malloc(sizeof(int)*2*db1->treads,"Allocating scaffold map");
      ALEN = (int *) Malloc(sizeof(int)*db1->nreads,"Allocating scaffold length map");
    }
    if (AMAP == NULL || ALEN == NULL)
      die("%s: Allocating scaffold map memory failed",Prog_Name);
    AOFF = AMAP + db1->treads;

    s = -1;
    for (r = 0; r < db1->treads; r++)
      { if (db1->reads[r].origin == 0)
          s += 1;
        AOFF[r] = db1->reads[r].fpulse;
        AMAP[r] = s;
        ALEN[s] = AOFF[r] + db1->reads[r].rlen;
      }

    if (ISTWO)
      { BMAP = AOFF + db1->treads;
        BOFF = BMAP + db2->treads;
        BLEN = ALEN + db1->nreads;

        s = -1;
        for (r = 0; r < db2->treads; r++)
          { if (db2->reads[r].origin == 0)
              s += 1;
            BOFF[r] = db2->reads[r].fpulse;
            BMAP[r] = s;
            BLEN[s] = BOFF[r] + db2->reads[r].rlen;
          }
      }
    else
      { BMAP = AMAP;
        BOFF = AOFF;
        BLEN = ALEN;
      }
  }

  //  Preload all scaffold headers and set header offset

  { int r, hdrs;
    int coff;
    char *HEADER, *eptr;
    struct stat state;

    hdrs = open(Catenate(db1->path,".hdr","",""),O_RDONLY);
    if (hdrs < 0)
      die("%s: Could not open header file of %s",Prog_Name,db1->path);
    if (fstat(hdrs,&state) < 0)
      die("%s: Could not fetch size of %s's header file",Prog_Name,db1->path);

    HEADER = Malloc(state.st_size,"Allocating header table");
    if (HEADER == NULL)
      die("%s: Allocating header table memory failed",Prog_Name);

    if (read(hdrs,HEADER,state.st_size) < 0)
      die("%s: Could not read header file of %s",Prog_Name,db1->path);
    
    HEADER[state.st_size-1] = '\0';
    close(hdrs);

    for (r = 1; r < db1->nreads; r++)
      if (db1->reads[r].origin == 0)
        HEADER[db1->reads[r].coff-1] = '\0';
    
    Adict = dictCreate(db1->nreads);
    for (r = 0; r < db1->nreads; r++)
      { coff = db1->reads[r].coff;
        eptr = HEADER + coff;
        do
          {
            if (isspace(*eptr))
              *eptr = '\0';
          }
        while(*eptr++ != '\0');
        dictAdd(Adict, HEADER + coff, NULL);
      }

    free(HEADER);

    if (ISTWO)
      { hdrs = open(Catenate(db2->path,".hdr","",""),O_RDONLY);
        if (hdrs < 0)
          die("%s: Could not open header file of %s",Prog_Name,db2->path);
        
        if (fstat(hdrs,&state) < 0)
          die("%s: Could not fetch size of %s's header file",Prog_Name,db2->path);

        HEADER = Malloc(state.st_size,"Allocating header table");
        if (HEADER == NULL)
          die("%s: Allocating header table memory failed",Prog_Name);

        if (read(hdrs,HEADER,state.st_size) < 0)
          die("%s: Could not read header file of %s",Prog_Name,db2->path);
        
        HEADER[state.st_size-1] = '\0';
        close(hdrs);

        for (r = 1; r < db2->nreads; r++)
          if (db2->reads[r].origin == 0)
            HEADER[db2->reads[r].coff-1] = '\0';
        
        Bdict = dictCreate(db2->nreads);
        for (r = 0; r < db2->nreads; r++)
          { coff = db2->reads[r].coff;
            eptr = HEADER + coff;
            do
              {
                if (isspace(*eptr))
                  *eptr = '\0';
              }
            while(*eptr++ != '\0');
            dictAdd(Bdict, HEADER + coff, NULL);
          }

        free(HEADER);
      }
    else
      Bdict = Adict;
  }

  Close_DB(db1);
  if (ISTWO)
    Close_DB(db2);

  return;
}

void *read_1aln_block(void *args)
{ Packet *parm  = (Packet *) args;
  int64    beg  = parm->beg;
  int64    end  = parm->end;
  OneFile *in   = parm->in;
  Segment *segs = parm->segs;
  int64    nseg = 0;

  //  For each alignment do

  if (!oneGotoObject (parm->in, beg))
    die("%s: Could not locate to object %lld in 1aln file",Prog_Name,beg);
  
  oneReadLine(parm->in);

  { int64 i;
    uint32   flags;
    int      aread, bread;
    int      abpos, bbpos;
    int      aepos, bepos;
    int      diffs;
    int      blocksum, iid;

    for (i = beg; i < end; i++)
      { // read i-th alignment record 
        if (in->lineType != 'A')
          die("%s: Failed to be at start of alignment",Prog_Name);

        flags = 0;
        aread = oneInt(in,0);
        abpos = oneInt(in,1);
        aepos = oneInt(in,2);
        bread = oneInt(in,3);
        bbpos = oneInt(in,4);
        bepos = oneInt(in,5);

        diffs = 0;
        while (oneReadLine(in))
          if (in->lineType == 'R')
            flags |= COMP_FLAG;
          else if (in->lineType == 'D')
            diffs = oneInt(in,0);
          else if (in->lineType == 'A')
            break; // stop at next A-line
          else
            continue; // skip trace lines
      
        if (aepos - abpos < MINALEN || bepos - bbpos < MINALEN)
          continue; // filter by segment size

        blocksum = (aepos-abpos) + (bepos-bbpos);
        iid      = (blocksum - diffs) / 2;
        if (2.*iid / blocksum < MINAIDNT)
          continue; // filter by segment identity

        // map to scaffold coordinates
        abpos += AOFF[aread];
        aepos += AOFF[aread];
        aread  = AMAP[aread];
        bbpos += BOFF[bread];
        bepos += BOFF[bread];
        bread  = BMAP[bread];
        if (COMP(flags))
          { bbpos = BLEN[bread] - bbpos;
            bepos = BLEN[bread] - bepos;
          }
        
        // add to output
        segs->aread = aread;
        segs->abpos = abpos;
        segs->aepos = aepos;
        segs->bread = bread;
        segs->bbpos = bbpos;
        segs->bepos = bepos;
        segs++;
        nseg++;
      }
  }
  parm->nseg = nseg;

  return (NULL);
}

void read_1aln(char *oneAlnFile)
{ Packet    *parm;
  OneFile   *input;
  int64      novl;

  //  Initiate .1aln file reading and read header information

  { char       *pwd, *root, *cpath;
    FILE       *test;
    char      *db1_name;
    char      *db2_name;
    int        TSPACE;
    
    pwd   = PathTo(oneAlnFile);
    root  = Root(oneAlnFile,".1aln");
    input = open_Aln_Read(Catenate(pwd,"/",root,".1aln"),NTHREADS,
                          &novl,&TSPACE,&db1_name,&db2_name,&cpath);
    if (input == NULL)
      die("%s: Could not open .1aln file: %s",Prog_Name,oneAlnFile);
    free(root);
    free(pwd);

    test = fopen(db1_name,"r");
    if (test == NULL)
      { if (*db1_name != '/')
          test = fopen(Catenate(cpath,"/",db1_name,""),"r");

        if (test == NULL)
          die("%s: Could not find .gdb %s",Prog_Name,db1_name);
        
        pwd = Strdup(Catenate(cpath,"/",db1_name,""),"Allocating expanded name");
        free(db1_name);
        db1_name = pwd;
      }
    fclose(test);
  
    if (db2_name != NULL)
      { test = fopen(db2_name,"r");
        if (test == NULL)
          { if (*db2_name != '/')
              test = fopen(Catenate(cpath,"/",db2_name,""),"r");
            
            if (test == NULL)
              die("%s: Could not find .gdb %s",Prog_Name,db2_name);
            
            pwd = Strdup(Catenate(cpath,"/",db2_name,""),"Allocating expanded name");
            free(db2_name);
            db2_name = pwd;
          }
        fclose(test);
      }

    makeSeqDICTFromDB(db1_name, db2_name);

    free(cpath);
    free(db1_name);
    free(db2_name);
  }

#ifdef DEBUG_DICT
  { uint64 i, index;
    for (i = 0; i < Adict->max; i++)
      fprintf(stderr, "#AdictName %4llu %8d %s\n", i, ALEN[i], dictName(Adict, i));
    for (i = 0; i < Bdict->max; i++)
      fprintf(stderr, "#BdictName %4llu %8d %s\n", i, BLEN[i], dictName(Bdict, i));

    for (i = 0; i < Adict->max; i++) {
      dictFind(Adict, dictName(Adict, i), &index);
      fprintf(stderr, "#AdictFind %s %4llu\n", dictName(Adict, i), index);
    }
    for (i = 0; i < Bdict->max; i++) {
      dictFind(Bdict, dictName(Bdict, i), &index);
      fprintf(stderr, "#BdictFind %s %4llu\n", dictName(Bdict, i), index);
    }
  }
#endif

  // Read alignment segments

  //  Divide .1aln into NTHREADS parts
  { int p;

    parm = Malloc(sizeof(Packet)*NTHREADS,"Allocating thread records");
    if (parm == NULL)
      die("%s: Allocating parm memory failed",Prog_Name);

    segments = (Segment *) Malloc(sizeof(Segment)*novl, "Allocating segment array");
    if (segments == NULL)
        die("%s: Allocating segment array memory failed",Prog_Name);
    for (p = 0; p < NTHREADS ; p++)
      { parm[p].beg = (p * novl) / NTHREADS;
        if (p > 0)
          parm[p-1].end = parm[p].beg;
        parm[p].segs = segments + parm[p].beg;
        parm[p].nseg = 0;
      }
    parm[NTHREADS-1].end = novl;
  }

  //   Use NTHREADS to produce alignment segments for each part
  {
    int p;
    int64 totSeg;
    pthread_t threads[NTHREADS];
    for (p = 0; p < NTHREADS; p++)
      parm[p].in   = input + p;
    for (p = 1; p < NTHREADS; p++)
      pthread_create(threads+p,NULL,read_1aln_block,parm+p);
    read_1aln_block(parm);
    for (p = 1; p < NTHREADS; p++)
      pthread_join(threads[p],NULL);
  
    // collect results from different part
    nSegment = parm[0].nseg;
    totSeg   = parm[0].end - parm[0].beg;
    for (p = 1; p < NTHREADS; p++)
      { if (nSegment < totSeg)
          memmove(parm[p].segs,segments+nSegment,parm[p].nseg);
        nSegment += parm[p].nseg;
        totSeg   += parm[p].end - parm[p].beg;
      }
#ifdef DEBUG_READ_1ALN
    fprintf(stderr, "%s: %9lld segments loaded\n",Prog_Name,totSeg);
    fprintf(stderr, "%s: %9lld after filtering\n",Prog_Name,nSegment);
#endif
  }

  free(parm);
  free(AMAP);

  oneFileClose(input);
}

#define LINE_SEP '\n'

typedef struct
  { uint64 n, m;
    char *buf;
  } Buffer;

Buffer *newBuffer(uint64 size)
{ Buffer *buffer;
  char *buf;

  if (size < 16) size = 16;

  buffer = (Buffer *) Malloc(sizeof(Buffer), "Allocating buffer memory");
  buf = (char *) Malloc(sizeof(char) * size, "Allocating buffer memory");
  
  if (buffer == NULL || buf == NULL)
    die("%s: Allocating buffer memory failed",Prog_Name);

  buffer->n = 0;
  buffer->m = size;
  buffer->buf = buf;
 
  return buffer;
}

void extendBuffer(Buffer *buffer)
{ buffer->m <<= 1;
  buffer->buf = (char *) Realloc(buffer->buf,sizeof(char)*buffer->m,"Extending buffer memory");
  if (buffer->buf == NULL)
    die("%s: Extending buffer size failed",Prog_Name);
}

void destroyBuffer(Buffer *buffer)
{ free(buffer->buf);
  free(buffer);
}

int get_until(void *input, int gzipd, Buffer *buffer)
{ uint64 pos, len;
  int eof;
  char *buf;

  eof = 0;
  buffer->n = 0;
  buffer->buf[0] = '\0';
  pos = 0;
  while (1)
    { buf = buffer->buf;
      if (gzipd)
        eof = (gzgets(input,buf+pos,buffer->m-pos) == NULL);
      else
        eof = (fgets(buf+pos,buffer->m-pos,input) == NULL);

      for (len = pos; buf[len] != LINE_SEP && buf[len]; len++) {}

      if (!eof && len == buffer->m-1)
        { extendBuffer(buffer);
          pos = len;
        }
      else
        break;
    }

  return eof;
}

void read_paf(char *pafAlnFile)
{ void *input;
  Buffer *buffer;
  int i, eof, gzipd, absent;
  uint64 index, naseq, maseq, nbseq, mbseq, nsegs, msegs;
  Segment *segs;
  char *fptrs[11], *fptr, *eptr;
  int alen, blen, *alens, *blens;;
  int aread, bread;
  int abpos, bbpos;
  int aepos, bepos;
  int blocksum, iid;

  gzipd = strlen(pafAlnFile) >= 3 && !strncmp(&pafAlnFile[strlen(pafAlnFile)-3], ".gz", 3);
  if (gzipd)
    input = gzopen(pafAlnFile,"r");
  else
    input = fopen(pafAlnFile,"r");

  if (input == NULL)
    die("%s: Could not find PAF file %s",Prog_Name,pafAlnFile);

  Adict = dictCreate(1024);
  Bdict = dictCreate(1024);
  
  naseq = 0;
  maseq = 1024;
  nbseq = 0;
  mbseq = 1024;
  nsegs = 0;
  msegs = 4096;

  alens = (int *)Malloc(sizeof(int) * maseq, "Allocating length map");
  blens = (int *)Malloc(sizeof(int) * mbseq, "Allocating length map");
  segments = (Segment *)Malloc(sizeof(Segment) * msegs, "Allocating segment array");
  if (alens == NULL || blens == NULL || segments == NULL)
    die("%s: Allocating memory failed",Prog_Name);
  segs = segments;

  buffer = newBuffer(0);
  while (1)
    { eof = get_until(input, gzipd, buffer);
      // parse PAF line
      eptr = buffer->buf;
      fptr = eptr;
      for (i = 0; i < 11; i++)
        { while (*eptr != '\t' && *eptr != '\n' && *eptr != '\0')
            eptr++;
          if (eptr > fptr)
            fptrs[i] = fptr;

          if (*eptr == '\t')
            { *eptr = '\0';
              fptr = ++eptr;
            }
          else
            { *eptr = '\0';
              break; 
            }
        }
      
      if (i == 11)
      { absent = dictAdd(Adict, fptrs[0], &index);
        alen   = strtol(fptrs[1], &eptr, 10);
        if (absent)
          { alens[naseq++] = alen;
            if (naseq == maseq)
              { maseq <<= 1;
                alens = (int *) Realloc(alens, sizeof(int) * maseq, "Reallocating length map");
                if (alens == NULL)
                  die("%s: Reallocating length map memory failed",Prog_Name);
              }
          }
        aread  = index;
        abpos  = strtol(fptrs[2], &eptr, 10);
        aepos  = strtol(fptrs[3], &eptr, 10);
      
        absent = dictAdd(Bdict, fptrs[5], &index);
        blen   = strtol(fptrs[6], &eptr, 10);
        if (absent)
          { blens[nbseq++] = blen;
            if (nbseq == mbseq)
              { mbseq <<= 1;
                blens = (int *) Realloc(blens, sizeof(int) * mbseq, "Reallocating length map");
                if (blens == NULL)
                  die("%s: Reallocating length map memory failed",Prog_Name);
              }
          }
        bread  = index;
        bbpos  = strtol(fptrs[7], &eptr, 10);
        bepos  = strtol(fptrs[8], &eptr, 10);
        
        absent = 0;
        if (aepos - abpos < MINALEN || bepos - bbpos < MINALEN)
          absent = 1; // filter by segment size

        blocksum = (aepos-abpos) + (bepos-bbpos);
        iid      = strtol(fptrs[9], &eptr, 10);
        if (2.*iid / blocksum < MINAIDNT)
          absent = 1; // filter by segment identity

        if (!absent)
          { // Add to segment array
            if (*fptrs[4] == '-')
              { int tmp = bbpos;
                bbpos = bepos;
                bepos = tmp;
              }
  
            segs->aread = aread;
            segs->bread = bread;
            segs->abpos = abpos;
            segs->aepos = aepos;
            segs->bbpos = bbpos;
            segs->bepos = bepos;
        
            nsegs++;
            if (nsegs == msegs)
              { msegs <<= 1;
                segments = (Segment *) Realloc(segments, sizeof(Segment) * msegs, "Reallocating segment array");
                if (segments == NULL)
                  die("%s: Reallocating segment array memory failed",Prog_Name);
              }
            segs = segments + nsegs;
          }
      }
      if (eof) break;
    }
 
  if (gzipd)
    gzclose(input);
  else
    fclose(input);

  alens = (int *) Realloc(alens, sizeof(int) * (naseq + nbseq), "Reallocating length map");
  if (alens == NULL)
    die("%s: Reallocating length map memory failed",Prog_Name);
  memcpy(alens + naseq, blens, sizeof(int) * nbseq);
  ALEN = alens;
  BLEN = ALEN + naseq;

  ISTWO = 1;
  nSegment = nsegs;

  free(blens);
  destroyBuffer(buffer);
}

int *parseTargetSEQ(char *seqStr, DICT *dict, int *slen)
{ int *SEQ;

  SEQ = (int *)Malloc(sizeof(int)*dict->max,"Allocating SEQ array");
  if (SEQ == NULL)
    die("%s: failed to allocate SEQ array memory",Prog_Name);
  
  if (seqStr == NULL)
    { // add all sequences
      uint64 i;
      SEQ[0] = 0;
      for (i = 1; i < dict->max; i++)
        SEQ[i] = SEQ[i-1] + slen[i-1];
      return SEQ;
    }

  if (*seqStr == '\0')
    die("%s: empty -x/-y parameter",Prog_Name);

  int *seqs;
  uint64 nseq, index;
  char *eptr, c;
  int found;

  seqs = (int *)Malloc(sizeof(int)*dict->max,"Allocating SEQ array");
  if (seqs == NULL)
    die("%s: failed to allocate SEQ array memory",Prog_Name);
  
  nseq = 0;

  if (*seqStr == NUM_SYMBOL)
    { seqStr++;
      while (*seqStr)
        { if (isdigit(*seqStr))
            { index = strtol(seqStr, &seqStr, 10);
              if (index == 0 || index > dict->max)
                die("%s: sequence index %lld is out of range 1-%lld",Prog_Name,index,dict->max);
              seqs[nseq++] = index-1;
            }
          else
            seqStr++;
        }
    }
  else if (*seqStr == FIL_SYMBOL)
    { seqStr++;
      die("%s: file input for -x/-y is not supported yet");
    }
  else
    { eptr = seqStr;
      while (1)
        { while (*eptr != SEP_SYMBOL && *eptr != '\0')
            eptr++;
          c = *eptr;
          *eptr = '\0';
          found = dictFind(dict, seqStr, &index);
          if (found)
            seqs[nseq++] = index;
          else if (strlen(seqStr))
            die("%s: sequence not found - %s",Prog_Name,seqStr);
          if (c == '\0')
            break;
          seqStr = ++eptr;
        }
    }

  if (!nseq)
    die("%s: no valid sequence specified for ploting -x/-y",Prog_Name);

#ifdef DEBUG_PARSE_SEQ
  { uint64 i;
    for (i = 0; i < nseq; i++)
      fprintf(stderr, "%s: sequence to plot - %4d %s\n",Prog_Name,seqs[i],dictName(dict,seqs[i]));
  }
#endif

  uint64 i; 
  for (i = 0; i < dict->max; i++)
    SEQ[i] = -1;
  SEQ[seqs[0]] = 0;
  for (i = 1; i < nseq; i++)
    { if (SEQ[seqs[i]] >= 0)
        die("%s: dupicate sequence in -x/-y parameter", Prog_Name);
      SEQ[seqs[i]] = SEQ[seqs[i-1]] + slen[seqs[i-1]];
    }
  free(seqs);

  return SEQ;
}

static int USORT(const void *l, const void *r)
{ uint64 *x = (uint64 *) l;
  uint64 *y = (uint64 *) r;

  return (*x > *y) - (*x < *y);
}

int *axisConfig(DICT *dict, int *slen, int *aoff, int *nseq, int64 *tseq)
{ uint64 i, n, *sarray;
  int *seqs;
  int64 t;

  n = 0;
  t = 0;
  for (i = 0; i < dict->max; i++)
    if (aoff[i] >= 0)
      { n++;
        t += slen[i];
      }
  
  sarray = (uint64 *)Malloc(sizeof(uint64)*n,"Allocating seq array");
  if (sarray == NULL)
    die("%s: Allocating seq array failed",Prog_Name);

  n = 0;
  for (i = 0; i < dict->max; i++)
    if (aoff[i] >= 0)
      sarray[n++] = (uint64) aoff[i] << 32 | i;

  qsort(sarray, n, sizeof(uint64), USORT);

  seqs = (int *)Malloc(sizeof(int)*n,"Allocating seq array");
  if (seqs == NULL)
    die("%s: Allocating seq array failed",Prog_Name);

  for (i = 0; i < n; i++)
    seqs[i] = (uint32) sarray[i];
  
  free(sarray);

  if (nseq) *nseq = n;
  if (tseq) *tseq = t;

  return seqs;
}

#define eps_header(fp,x,y,linewidth) { \
    fprintf(fp,"%%!PS-Adobe-3.0 EPSF-3.0\n"); \
    fprintf(fp,"%%%%BoundingBox:"); \
    fprintf(fp," 1 1 %g %g\n\n",(float)(x),(float)(y)); \
    fprintf(fp,"/C { dup 255 and 255 div exch dup -8 bitshift 255 and 255 div 3 1 roll -16 bitshift 255 and 255 div 3 1 roll setrgbcolor } bind def\n"); \
    fprintf(fp,"/L { 4 2 roll moveto lineto } bind def\n"); \
    fprintf(fp,"/LX { dup 4 -1 roll exch moveto lineto } bind def\n"); \
    fprintf(fp,"/LY { dup 4 -1 roll moveto exch lineto } bind def\n"); \
    fprintf(fp,"/LS { 3 1 roll moveto show } bind def\n"); \
    fprintf(fp,"/MS { dup stringwidth pop 2 div 4 -1 roll exch sub 3 -1 roll moveto show } bind def\n"); \
    fprintf(fp,"/RS { dup stringwidth pop 4 -1 roll exch sub 3 -1 roll moveto show } bind def\n"); \
    fprintf(fp,"/B { 4 copy 3 1 roll exch 6 2 roll 8 -2 roll moveto lineto lineto lineto closepath } bind def\n");\
    fprintf(fp,"%g setlinewidth\n\n",linewidth);\
}
#define eps_font(fp,f,s) do { \
    fprintf(fp,"/FS %d def\n",s); \
    fprintf(fp,"/FS4 FS 4 div def\n"); \
    fprintf(fp,"/%s findfont FS scalefont setfont\n\n",f); \
} while (0)

#define eps_bottom(fp) fprintf(fp,"stroke showpage\n")
#define eps_color(fp,col) fprintf(fp,"stroke %d C\n",col)
#define eps_gray(fp,gray) fprintf(fp, "%g setgray\n",(float)gray)
#define eps_linewidth(fp, lw) fprintf(fp, "%g setlinewidth\n", (float)(lw))
#define eps_line(fp,x1,y1,x2,y2) fprintf(fp,"%g %g %g %g L\n",(float)(x1),(float)(y1),(float)(x2),(float)(y2))
#define eps_linex(fp,x1,x2,y) fprintf(fp,"%g %g %g LX\n",(float)(x1),(float)(x2),(float)(y))
#define eps_liney(fp,y1,y2,x) fprintf(fp,"%g %g %g LY\n",(float)(y1),(float)(y2),(float)(x))
#define eps_Mstr(fp,x,y,s) fprintf(fp,"%g %g (%s) MS\n",(float)(x),(float)(y),s)
#define eps_Mint(fp,x,y,i) fprintf(fp,"%g %g (%d) MS\n",(float)(x),(float)(y),i)
#define eps_stroke(fp) fprintf(fp,"stroke\n")

#define N_COLOR 0xFF0000
#define C_COLOR 0x0080FF

static int SEG_COLOR[2] = {N_COLOR, C_COLOR};

void make_plot(FILE *fo)
{ // generate eps file
  int i, c;
  int width, height, lsize;
  int *xseqs, *yseqs;
  int nxseq, nyseq;
  int64 txseq, tyseq;
  double sx, sy;

  // find total length of x- and y-axis and order of plotting
  xseqs = yseqs = NULL;
  nxseq = nyseq = 0;
  txseq = tyseq = 0;
  xseqs = axisConfig(Bdict, BLEN, BSEQ, &nxseq, &txseq);
  yseqs = axisConfig(Adict, ALEN, ASEQ, &nyseq, &tyseq);

  width  = IMGWIDTH;
  height = IMGHEIGH;
  if (!height)
    height = (int)((double) width / txseq * tyseq + .499);
  if (!width)
    width = (int)((double) height / tyseq * txseq + .499);
  
  lsize = width > height? width : height;
  if (lsize > MAX_XY_LEN)
    { double scale = (double) MAX_XY_LEN / lsize;
      warn("%s: image size too large [%d]x[%d]",Prog_Name,width,height);
      width  = (int)(width  * scale + 0.499);
      height = (int)(height * scale + 0.499);
      warn("%s: shrink the size to [%d]x[%d]",Prog_Name,width,height);
      
      if (width < MIN_XY_LEN)
        { warn("%s: image width too small [%d]",Prog_Name,width);
          warn("%s: reset image width to [%d]",Prog_Name,MIN_XY_LEN);
          warn("%s: image and sequence size are not in proportion",Prog_Name);
          width = MIN_XY_LEN;  
        }
      if (height < MIN_XY_LEN)
        { warn("%s: image height too small [%d]",Prog_Name,height);
          warn("%s: reset image height to [%d]",Prog_Name,MIN_XY_LEN);
          warn("%s: image and sequence size are not in proportion",Prog_Name);
          height = MIN_XY_LEN;
        }
    }

  lsize = width < height? width : height;
  if (lsize < MIN_XY_LEN)
    { double scale = (double) MIN_XY_LEN / lsize;
      warn("%s: image size too small [%d]x[%d]",Prog_Name,width,height);
      width  = (int)(width  * scale + 0.499);
      height = (int)(height * scale + 0.499);
      warn("%s: rescale the size to [%d]x[%d]",Prog_Name,width,height);

      if (width > MAX_XY_LEN)
        { warn("%s: image width too large [%d]",Prog_Name,width);
          warn("%s: reset image width to [%d]",Prog_Name,MAX_XY_LEN);
          warn("%s: image and sequence size are not in proportion",Prog_Name);
          width = MAX_XY_LEN;
        }
      if (height > MAX_XY_LEN)
        { warn("%s: image height too large [%d]",Prog_Name,height);
          warn("%s: reset image height to [%d]",Prog_Name,MAX_XY_LEN);
          warn("%s: image and sequence size are not in proportion",Prog_Name);
          height = MAX_XY_LEN;
        }
    }

  sx = (double)  width / txseq;
  sy = (double) height / tyseq;

  eps_header(fo, width, height, .2);
  eps_font(fo, "Helvetica-Narrow", FONTSIZE);
  eps_gray(fo, .8);

  if (!NOLABEL)
    { // write x labels
      if (PRINTSID)
        for (i = 0; i < nxseq; i++)
          eps_Mint(fo, (BSEQ[xseqs[i]] + .5 * BLEN[xseqs[i]]) * sx, FONTSIZE * .5, xseqs[i] + 1);
      else
        for (i = 0; i < nxseq; i++)
          eps_Mstr(fo, (BSEQ[xseqs[i]] + .5 * BLEN[xseqs[i]]) * sx, FONTSIZE * .5, dictName(Bdict, xseqs[i]));
      eps_stroke(fo);
      fprintf(fo, "gsave %g 0 translate 90 rotate\n", FONTSIZE * 1.25);
      
      // write y labels
      if (PRINTSID)
        for (i = 0; i < nyseq; i++)
          eps_Mint(fo, (ASEQ[yseqs[i]] + .5 * ALEN[yseqs[i]]) * sy, 0, yseqs[i] + 1);
      else
        for (i = 0; i < nyseq; i++)
          eps_Mstr(fo, (ASEQ[yseqs[i]] + .5 * ALEN[yseqs[i]]) * sy, 0, dictName(Adict, yseqs[i]));
      fprintf(fo, "grestore\n");
      eps_stroke(fo);
    }

  // write grid lines
  eps_linewidth(fo, .1);
  for (i = 0; i < nyseq; i++)
    eps_linex(fo, 1, width,  i == 0? 1 : ASEQ[yseqs[i]] * sy);
  eps_linex(fo, 1, width,  tyseq * sy);
  for (i = 0; i < nxseq; i++)
    eps_liney(fo, 1, height, i == 0? 1 : BSEQ[xseqs[i]] * sx);
  eps_liney(fo, 1, height, txseq * sx);
  eps_stroke(fo);

  // write segments
  int aread, bread;
  double x0, y0, x1, y1, xo, yo;
  Segment *segment;
  eps_linewidth(fo, .1);
  for (c = 0; c < 2; c++)
    { eps_color(fo, SEG_COLOR[c]);
      for (i = 0; i < nSegment; i++)
        { segment = &segments[i];
          aread = segment->aread;
          bread = segment->bread;
          xo = BSEQ[bread];
          yo = ASEQ[aread];
          if (xo < 0 || yo < 0)
            continue;
          x0 = segment->bbpos;
          x1 = segment->bepos;
          y0 = segment->abpos;
          y1 = segment->aepos;
          if (c == 0 && x0 > x1) continue;
          if (c == 1 && x0 < x1) continue;
          x0 = (x0 + xo) * sx;
          x1 = (x1 + xo) * sx;
          y0 = (y0 + yo) * sy;
          y1 = (y1 + yo) * sy;
          eps_line(fo, x0, y0, x1, y1);
        }
      eps_stroke(fo);
    }
  eps_bottom(fo);
  
  free(xseqs);
  free(yseqs);
}

int main(int argc, char *argv[])
{ //  Process options

  int    i, j, k;
  int    flags[128];
  char  *xseq, *yseq, *output;
  char  *eptr;
  FILE  *foeps;

  ARG_INIT("ALNplot")
    
  xseq   = NULL;
  yseq   = NULL;
  output = NULL;
  j = 1;
  for (i = 1; i < argc; i++)
    if (argv[i][0] == '-')
      switch (argv[i][1])
        { default:
            ARG_FLAGS("dhpSL")
            break;
          case 'l':
            ARG_NON_NEGATIVE(MINALEN,"Minimum alignment length")
            break;
          case 'i':
            ARG_REAL(MINAIDNT)
            break;
          case 'H':
            ARG_POSITIVE(IMGHEIGH,"Image height")
            break;
          case 'W':
            ARG_POSITIVE(IMGWIDTH,"Image width")
            break;
          case 'f':
            ARG_POSITIVE(FONTSIZE,"Label font size")
            break;
          case 'x':
            xseq = argv[i]+2;
            break;
          case 'y':
            yseq = argv[i]+2;
            break;
          case 'T':
            ARG_POSITIVE(NTHREADS,"Number of threads")
            break;
          case 'o':
            output = argv[i]+2;
            break;
        }
      else
        argv[j++] = argv[i];
  argc = j;

  if (argc != 2 || flags['h'])
    { fprintf(stderr,"\nUsage: %s %s\n",Prog_Name,Usage[0]);
      fprintf(stderr,"       %*s %s\n",(int) strlen(Prog_Name),"",Usage[1]);
      fprintf(stderr,"       %*s %s\n",(int) strlen(Prog_Name),"",Usage[2]);
      fprintf(stderr,"\n");
      fprintf(stderr,"       <target> = <string>[,<string>[,...]] | #<int>[,<int>[,...]] | @<FILE>\n");
      fprintf(stderr,"\n");
      fprintf(stderr,"      -l: minimum alignment length\n");
      fprintf(stderr,"      -i: minimum alignment sequence identity\n");
      fprintf(stderr,"      -x: sequences placed on x-axis\n");
      fprintf(stderr,"      -y: sequences placed on y-axis\n");
      fprintf(stderr,"      -d: try to put alignments along the diagonal line\n");
      fprintf(stderr,"      -S: print sequence IDs as labels instead of names\n");
      fprintf(stderr,"      -L: do not print labels\n");
      fprintf(stderr,"      -H: image height\n");
      fprintf(stderr,"      -W: image width\n");
      fprintf(stderr,"      -f: label font size\n");
      fprintf(stderr,"      -T: use -T threads\n");
      fprintf(stderr,"\n");
      fprintf(stderr,"      -p: input is PAF format\n");
      fprintf(stderr,"      -o: make PDF output (requires \'epstopdf\')\n");
      fprintf(stderr,"\n");
      if (flags['h'])
        exit (0);
      else
        exit (1);
    }

  TRYADIAG = flags['d'];
  PAFINPUT = flags['p'];
  PRINTSID = flags['S'];
  NOLABEL  = flags['L'];

  if (TRYADIAG)
    die("%s: diagonalisation (-d) is not supported yet",Prog_Name);

  if (IMGWIDTH && IMGHEIGH)
    warn("%s: setting both image width and height is not recommended",Prog_Name);

  if (!IMGWIDTH && !IMGHEIGH) IMGHEIGH = 600;

  if (output)
    { if (*output == '\0')
        die("%s: empty output file name",Prog_Name);

      char *pwd, *root;
      pwd    = PathTo(output);
      root   = Root(output,".pdf");
      OUTPDF = strdup(Catenate(pwd,"/",root,".pdf"));
      OUTEPS = strdup(Catenate(pwd,"/",root,".eps"));
      free(pwd);
      free(root);
    }

  PAFINPUT? read_paf(argv[1]) : read_1aln(argv[1]);
  
  ASEQ = parseTargetSEQ(yseq, Adict, ALEN);
  BSEQ = parseTargetSEQ(xseq, Bdict, BLEN);

  foeps = OUTEPS? fopen(OUTEPS,"w") : stdout;
  if (foeps == NULL)
    die("%s: Could not open file %s for writing",Prog_Name,OUTEPS);
  make_plot(foeps);
  if (foeps != stdout)
    fclose(foeps);
  
  if (OUTPDF != NULL && check_executable("epstopdf"))
    { char cmd[4096];
      sprintf(cmd, "epstopdf -o %s %s", OUTPDF, OUTEPS);
      run_system_cmd(cmd, 1);
    }

  free(ALEN);
  free(ASEQ);
  free(BSEQ);
  dictDestroy(Adict);
  if (ISTWO)
    dictDestroy(Bdict);

  free(segments);
  free(OUTPDF);
  free(OUTEPS);
  free(Prog_Name);

  Catenate(NULL,NULL,NULL,NULL);
  
  exit (0);
}

