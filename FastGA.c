 /*******************************************************************************************
 *
 *  Adaptamer merge phase of a WGA.  Given indices of two genomes (at a specified frequency
 *   cutoff), the adaptemer matches between the k-mers are found in a novel, cache-coherent
 *   merge of the sorted k-mer tables for each genome and seed position pairs are output for
 *   each adaptemer match.
 *
 *  Author:  Gene Myers
 *  Date  :  February 2023
 *
 *******************************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>

#include "libfastk.h"
#include "DB.h"
#include "align.h"

#undef    DEBUG_SPLIT
#undef    DEBUG_MERGE
#undef    DEBUG_SORT
#undef    DEBUG_SEARCH
#undef    DEBUG_HIT
#define   CALL_ALIGNER
#undef    DEBUG_ALIGN
#undef    DEBUG_ENTWINE

#define   MAX_INT64    0x7fffffffffffffffll

#define    TSPACE       100
static int TBYTES;      //  # of bytes per trace element
static int ABYTE;       //  TBYTES is 1

static int PTR_SIZE = sizeof(void *);
static int OVL_SIZE = sizeof(Overlap) - sizeof(void *);

#define    BUCK_WIDTH    64
#define    BUCK_SHIFT     6

static int    CHAIN_BREAK;
static int    CHAIN_MIN;
static int    ALIGN_MIN;
static double ALIGN_RATE;

static char *Usage[] = { "[-v] [-P<dir(/tmp)>] [-o<out:name>] -f<int>",
                         "[-c<int(100)> [-s<int(500)>] [-a<int(100)>] [-e<float(.7)]",
                         "<source1>[.dam] <source2>[.dam]"
                       };

static int   FREQ;     //  Adaptemer frequence cutoff parameter
static int   VERBOSE;  //  Verbose output
static int   KMER;
static int   NTHREADS;

static char *PAIR_NAME;
static char *ALGN_NAME;
static char *ALGN_UNIQ;
static char *ALGN_PAIR;
static char *SORT_PATH;

static int IBYTE;   // # of bytes for an entry in P1
static int IPOST;   // # of bytes in post of a P1 entry
static int ICONT;   // # of bytes in contig of a P1 entry
static int ISIGN;   // byte index in a P1 entry of the sign flag

static int JBYTE;   // # of bytes for an entry in P2
static int JPOST;   // # of bytes in post of a P2 entry
static int JCONT;   // # of bytes in contig of a P2 entry
static int JSIGN;   // byte index in a P2 entry of the sign flag

static int KBYTE;   // # of bytes for a k-mer table entry (both T1 & T2)
static int CBYTE;   // byte of k-mer table entry containing post count
static int LBYTE;   // byte of k-mer table entry containing lcp

static int DBYTE;   // # of bytes for a pair diagonal

static int    NCONTS;    //  # of A contigs
static int    NPARTS;    //  # of panels A-contigs divided into
static int    ESHIFT;    //  shift to extract P1-contig # from a post

static int   *Select;    //  Select[bucket] = thread file for bucket
static int   *IDBsplit;  //  DB split: contigs [DBsplit[p],DBsplit[p+1])
static int   *Perm1;     //  Sorted contig permutation of DB1
static int   *Perm2;     //  Sorted contig permutation of DB2

typedef struct
  { char  *name;
    uint8 *bufr;
    uint8 *btop;
    uint8 *bend;
    int64 *buck;
    int    file;
  } IOBuffer;

static IOBuffer *N_Units;  //  NTHREADS^2 IO units for + pair temporary files
static IOBuffer *C_Units;  //  NTHREADS^2 IO units for - pair temporary files

typedef struct
  { int   beg;
    int   end;
    int64 off;
  } Range;

extern int rmsd_sort(uint8 *array, int64 nelem, int rsize, int ksize,
                     int nparts, int64 *part, int nthreads, Range *range);


/***********************************************************************************************
 *
 *   POSITION LIST ABSTRACTION:
 *       Routines to manipuate the position or "post" list associated with a k-mer table.
 *
 **********************************************************************************************/

typedef struct
  { int     pbyte;      //  # of bytes for each post (including sign & contig)
    int     cbyte;      //  # of bytes for each sign & contig
    int64   nels;       //  # of posts in the index
    int64   maxp;
    int     freq;
    int     nctg;
    int    *perm;
    int64   cidx;
    uint8  *cache;
    uint8  *cptr;

    int     copn;       //  File currently open
    int     part;       //  Thread # of file currently open
    int     nthr;       //  # of file parts
    int     nsqrt;      //  # of threads/slices (= sqrt(nthr))
    int     nlen;       //  length of path name
    char   *name;       //  Path name for table parts (only # missing)
    uint8  *ctop;       //  Ptr top of current table block in buffer
    int64  *neps;       //  Size of each thread part in elements
  } Post_List;

#define POST_BLOCK 1024

//  Load up the table buffer with the next STREAM_BLOCK suffixes (if possible)

static void More_Post_List(Post_List *P)
{ int    pbyte = P->pbyte;
  uint8 *cache = P->cache;
  int    copn  = P->copn;
  uint8 *ctop;

  if (P->part > P->nthr)
    return;
  while (1)
    { ctop = cache + read(copn,cache,POST_BLOCK*pbyte);
      if (ctop > cache)
        break;
      close(copn);
      P->part += 1;
      if (P->part > P->nthr)
        { P->cptr = NULL;
          return;
        }
      sprintf(P->name+P->nlen,"%d",P->part);
      copn = open(P->name,O_RDONLY);
      lseek(copn,2*sizeof(int)+sizeof(int64),SEEK_SET);
    }
  P->cptr = cache;
  P->ctop = ctop;
  P->copn = copn;
}

static Post_List *Open_Post_List(char *name)
{ Post_List *P;
  int        pbyte, cbyte, nctg;
  int64      nels, maxp, n;
  int        copn;

  int    f, p, flen;
  char  *dir, *root, *full;
  int    pb, cb, nfile, nthreads, freq;

  dir  = PathTo(name);
  root = Root(name,".ktab");
  full = Malloc(strlen(dir)+strlen(root)+20,"Post list name allocation");
  if (full == NULL)
    exit (1);
  sprintf(full,"%s/%s.post",dir,root);
  f = open(full,O_RDONLY);
  sprintf(full,"%s/.%s.post.",dir,root);
  flen = strlen(full);
  free(root);
  free(dir);
  if (f < 0)
    { free(full);
      return (NULL);
    }

  read(f,&pbyte,sizeof(int));
  read(f,&cbyte,sizeof(int));
  pbyte += cbyte;

  read(f,&nfile,sizeof(int));
  read(f,&maxp,sizeof(int64));
  read(f,&freq,sizeof(int));
  nthreads = nfile;
  nfile    = nfile*nfile;

  read(f,&nctg,sizeof(int));

  P = Malloc(sizeof(Post_List),"Allocating post record");
  if (P == NULL)
    exit (1);
  P->name   = full;
  P->nlen   = strlen(full);
  P->maxp   = maxp;
  P->cache  = Malloc(POST_BLOCK*pbyte,"Allocating post list buffer\n");
  P->neps   = Malloc(nfile*sizeof(int64),"Allocating parts table of Post_List");
  P->perm   = Malloc(nctg*sizeof(int),"Allocating sort permutation");
  if (P->cache == NULL || P->neps == NULL || P->perm == NULL)
    exit (1);

  read(f,P->perm,sizeof(int)*nctg);

  nels = 0;
  for (p = 1; p <= nfile; p++)
    { sprintf(P->name+P->nlen,"%d",p);
      copn = open(P->name,O_RDONLY);
      if (copn < 0)
        { fprintf(stderr,"%s: Table part %s is missing ?\n",Prog_Name,P->name);
          exit (1);
        }
      read(copn,&pb,sizeof(int));
      read(copn,&cb,sizeof(int));
      pb += cb;
      read(copn,&n,sizeof(int64));
      nels += n;
      P->neps[p-1] = nels;
      if (pbyte != pb)
        { fprintf(stderr,"%s: Post list part %s does not have post size matching stub ?\n",
                         Prog_Name,P->name);
          exit (1);
        }
      close(copn);
    }

  P->pbyte = pbyte;
  P->cbyte = cbyte;
  P->nels  = nels;
  P->nthr  = nfile;
  P->nsqrt = nthreads;
  P->freq  = freq;
  P->nctg  = nctg;

  sprintf(P->name+P->nlen,"%d",1);
  copn = open(P->name,O_RDONLY);
  lseek(copn,2*sizeof(int)+sizeof(int64),SEEK_SET);

  P->copn  = copn;
  P->part  = 1;

  More_Post_List(P);
  P->cidx = 0;

  return (P);
}

static void Free_Post_List(Post_List *P)
{ free(P->neps);
  free(P->cache);
  free(P->perm);
  if (P->copn >= 0)
    close(P->copn);
  free(P->name);
  free(P);
}

static inline void First_Post_Entry(Post_List *P)
{ if (P->cidx != 0)
    { if (P->part != 1)
        { if (P->part <= P->nthr)
            close(P->copn);
          sprintf(P->name+P->nlen,"%d",1);
          P->copn = open(P->name,O_RDONLY);
          P->part = 1;
        }

      lseek(P->copn,sizeof(int)+sizeof(int64),SEEK_SET);

      More_Post_List(P);
      P->cidx = 0;
    }
}

static inline void Next_Post_Entry(Post_List *P)
{ P->cptr += P->pbyte;
  P->cidx += 1;
  if (P->cptr >= P->ctop)
    { if (P->cidx >= P->nels)
        { P->cptr = NULL;
          P->part = P->nthr+1;
          return;
        }
      More_Post_List(P);
    }
}

static inline void Current_Post(Post_List *P, uint8 *here)
{ memcpy(here,P->cptr,P->pbyte); }

static inline void GoTo_Post_Index(Post_List *P, int64 i)
{ int    p;

  if (P->cidx == i)
    return;
  P->cidx = i;

  p = 0;
  while (i >= P->neps[p])
    p += 1;

  if (p > 0)
    i -= P->neps[p-1];
  p += 1;

  if (P->part != p)
    { if (P->part <= P->nthr)
        close(P->copn);
      sprintf(P->name+P->nlen,"%d",p);
      P->copn = open(P->name,O_RDONLY);
      P->part = p;
    }

  lseek(P->copn,2*sizeof(int) + sizeof(int64) + i*P->pbyte,SEEK_SET);

  More_Post_List(P);
}

static inline void JumpTo_Post_Index(Post_List *P, int64 del)
{ int   p;
  int64 i;

  P->cptr += del*P->pbyte;
  P->cidx += del;
  if (P->cptr < P->ctop)
    return;

  i = P->cidx;
  p = P->part-1;
  while (i >= P->neps[p])
    p += 1;

  if (p > 0)
    i -= P->neps[p-1];
  p += 1;

  if (P->part != p)
    { if (P->part <= P->nthr)
        close(P->copn);
      sprintf(P->name+P->nlen,"%d",p);
      P->copn = open(P->name,O_RDONLY);
      P->part = p;
    }

  lseek(P->copn,2*sizeof(int) + sizeof(int64) + i*P->pbyte,SEEK_SET);

  More_Post_List(P);
}


/***********************************************************************************************
 *
 *   The internal data structure for a table (taken from libfastk.c) needs to be visible so
 *     that the "neps" array can be accessed in order to synchronize thread starts.
 *
 **********************************************************************************************/

typedef struct
  { int    kmer;       //  Kmer length
    int    minval;     //  The minimum count of a k-mer in the stream
    int64  nels;       //  # of elements in entire table
                   //  Current position (visible part)
    int64  cidx;       //  current element index
    uint8 *csuf;       //  current element suffix
    int    cpre;       //  current element prefix
                   //  Other useful parameters
    int    ibyte;      //  # of bytes in prefix
    int    kbyte;      //  Kmer encoding in bytes
    int    tbyte;      //  Kmer+count entry in bytes
    int    hbyte;      //  Kmer suffix in bytes (= kbyte - ibyte)
    int    pbyte;      //  Kmer,count suffix in bytes (= tbyte - ibyte)
                   //  Hidden parts
    int    ixlen;      //  length of prefix index (= 4^(4*ibyte))
    int    shift;      //  shift for inverse mapping
    uint8 *table;      //  The (huge) table in memory
    int64 *index;      //  Prefix compression index
    int   *inver;      //  inverse prefix index
    int    copn;       //  File currently open
    int    part;       //  Thread # of file currently open
    int    nthr;       //  # of thread parts
    int    nlen;       //  length of path name
    char  *name;       //  Path name for table parts (only # missing)
    uint8 *ctop;       //  Ptr top of current table block in buffer
    int64 *neps;       //  Size of each thread part in elements
    int    clone;      //  Is this a clone?
  } _Kmer_Stream;


/***********************************************************************************************
 *
 *   ADAPTAMER MERGE THREAD:  
 *     For each k-mer in T1
 *       Find the longest prefix match to one or more k-mers in T2.
 *       If the totwl # of positions of the k-mers in T2, then output the pairs of positions
 *         from P1 & P2 to a file dependent on the slice of the P1 post and the sign of the match.
 *
 **********************************************************************************************/

static int cbyte[41] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                         3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
                         6, 6, 6, 6, 7 };

static int mbyte[41] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         0xc0, 0x30, 0x0c, 0x03, 0xc0, 0x30, 0x0c, 0x03, 0xc0, 0x30, 0x0c, 0x03,
                         0xc0, 0x30, 0x0c, 0x03, 0xc0, 0x30, 0x0c, 0x03, 0xc0, 0x30, 0x0c, 0x03,
                         0xc0, 0x30, 0x0c, 0x03, 0xc0 };

#define POST_BUF_LEN  0x1000
#define POST_BUF_MASK 0x0fff

typedef struct
  { Kmer_Stream *T1;
    Kmer_Stream *T2;
    Post_List   *P1;
    Post_List   *P2;
    int          tid;
    uint8       *cache;
    IOBuffer    *nunit;
    IOBuffer    *cunit;
    int64        nhits;
    int64        g1len;
    int64        tseed;
  } SP;

static void *merge_thread(void *args)
{ SP *parm = (SP *) args;
  int tid          = parm->tid;
  uint8 *cache     = parm->cache;
  IOBuffer  *nunit = parm->nunit;
  IOBuffer  *cunit = parm->cunit;
  Kmer_Stream *T1  = parm->T1;
  Post_List   *P1  = parm->P1;
  Kmer_Stream *T2  = parm->T2;
  Post_List   *P2  = parm->P2;

  int     spart;
  int64   tbeg, tend;

  int     cpre;
  uint8  *ctop, *suf1;

  int     eorun, plen;
  uint8  *rcur, *rend;
  uint8  *vlcp[KMER+1];

  int64   apost;
  uint8  *aptr = (uint8 *) (&apost);

  uint8  *vlow, *vhgh;
  int     pdx, cdx;
  int64   post[POST_BUF_LEN + FREQ];

  int     qcnt, pcnt;
  int64   nhits, g1len, tseed;

#ifdef DEBUG_MERGE
  int64   Tdp;
  char   *tbuffer;
#endif

  { int j;

    for (j = 0; j < NPARTS; j++)
      { nunit[j].bend = nunit[j].bufr + (1000000-(IBYTE+JBYTE+1));
        nunit[j].btop = nunit[j].bufr;
        cunit[j].bend = cunit[j].bufr + (1000000-(IBYTE+JBYTE+1));
        cunit[j].btop = cunit[j].bufr;
      }
  }

  cpre  = -1;
  ctop  = cache;
  vhgh  = cache;
#ifdef DEBUG_MERGE
  tbuffer = Current_Kmer(T1,NULL);
#endif

  nhits = 0;
  g1len = 0;
  tseed = 0;
  apost = 0;

  spart = P1->nsqrt * tid - 1;
  First_Post_Entry(P1);
  First_Post_Entry(P2);
  First_Kmer_Entry(T1);
  First_Kmer_Entry(T2);

  if (tid != 0)
    { GoTo_Post_Index(P1,P1->neps[spart]);
      GoTo_Post_Index(P2,P2->neps[spart]);
      GoTo_Kmer_Index(T1,((_Kmer_Stream *) T1)->neps[spart]);
      GoTo_Kmer_Index(T2,((_Kmer_Stream *) T2)->neps[spart]);
    }

  qcnt = -1;
  tend = ((_Kmer_Stream *) T1)->neps[spart+P1->nsqrt];
  tbeg = T1->cidx;
  while (T1->cidx < tend)
    { suf1 = T1->csuf;
#ifdef DEBUG_MERGE
      printf("Doing %s (%lld)\n",Current_Kmer(T1,tbuffer),T1->cidx); fflush(stdout);
#endif

      if (T1->cpre != cpre)  //  New prefix panel
        { int64  bidx;
          uint8 *cp;

          if (VERBOSE && tid == 0)
            { pcnt = ((T1->cidx - tbeg) * 100) / (tend-tbeg); 
              if (pcnt > qcnt)
                { printf("\r    Completed %3d%%",pcnt);
                  fflush(stdout);
                }
              qcnt = pcnt;
            }

          //  skip remainder of cache and T2 entries < T1->cpre

          bidx = 0;
          for (cp = vhgh; cp < ctop; cp += KBYTE)
            bidx += cp[CBYTE];
          for (cpre = T1->cpre; T2->cpre < cpre; Next_Kmer_Entry(T2))
            bidx += T2->csuf[CBYTE];
          JumpTo_Post_Index(P2,bidx);

#ifdef DEBUG_MERGE
          Tdp = T2->cidx;
          printf("Loading %lld %06x ...",Tdp,cpre); fflush(stdout);
#endif

          //  load cahce with T2 entries = T1->cpre

          for (cp = cache; T2->cpre == cpre; cp += KBYTE)
            { memcpy(cp,T2->csuf,KBYTE);
              Next_Kmer_Entry(T2);
            }
          ctop = cp;
          ctop[LBYTE] = 11;

          //  if cache is empty then skip to next T1 entry with a greater prefix than cpre

          if (ctop == cache)
            { bidx = 0;
              while (T1->cpre == cpre)
                { bidx += T1->csuf[CBYTE];
                  Next_Kmer_Entry(T1);
                }
              JumpTo_Post_Index(P1,bidx);
#ifdef DEBUG_MERGE
              printf(" ... Empty => to %06x in T1\n",T1->cpre);
#endif
              continue;
            }

          //  start adpatermer merge for prefix cpre

          plen = 12;
          vlcp[plen] = rcur = rend = cache;
          vlow  = cache-KBYTE;
          vhgh  = cache;
          pdx   = POST_BUF_MASK;
          cdx   = 0;
          eorun = 0;

#ifdef DEBUG_MERGE
          printf("... to %lld rcur = rend = %lld, eorunn = 0, plen = 12\n",
                 T2->cidx,Tdp+(rcur-cache)/KBYTE);
          fflush(stdout);
#endif
        }

#define ADVANCE(l)					\
{ if (l >= vhgh)					\
    { int n;						\
							\
      for (n = l[CBYTE]; n > 0; n--)			\
        { pdx = (pdx+1) & POST_BUF_MASK;		\
          Current_Post(P2,(uint8 *) (post+pdx));	\
          Next_Post_Entry(P2);				\
        }						\
      vhgh = l+KBYTE;					\
    }							\
  cdx = (cdx + l[CBYTE]) & POST_BUF_MASK;		\
  l += KBYTE;						\
}

      // eorun = 0: rcur <= rend, n[1..plen] = rcur..rend, n[plen+1] < rend[plen+1]
      // eorun = 1: rcur <  rend, n[1..plen] = rcur..rend-1, lcp(rend) < plen 

      else
         { int nlcp;

           nlcp = suf1[LBYTE];
           if (nlcp > plen)
             goto pairs;
           else if (nlcp == plen)
             { if (eorun)
                 goto pairs;
             }
           else
             { if ( ! eorun)
                 ADVANCE(rend)
               while (rend[LBYTE] > nlcp)
                 ADVANCE(rend)
               plen = rend[LBYTE];
               if (plen < nlcp)
                 { eorun = 1;
                   plen  = nlcp;
                   goto pairs;
                 }
               eorun = 0;
               rcur  = rend;
             }
         }

       while (plen < KMER)
         { int h, m, c, d;

           h = cbyte[plen];
           m = mbyte[plen];
           c = suf1[h] & m;
           for (d = rend[h]&m; d < c; d = rend[h]&m)
             { ADVANCE(rend)
               if (rend[LBYTE] < plen)
                 { eorun = 1;
                   goto pairs;
                 }
             }
           if (d > c)
             goto pairs;
           plen += 1;
           vlcp[plen] = rcur = rend;
         }
       ADVANCE(rend)
       eorun = 1;

       //  Get pairs;

    pairs:

#ifdef DEBUG_MERGE
      printf("-> %d[%lld,%lld] %d",plen,Tdp+(vlcp[plen]-cache)/KBYTE,Tdp+(rend-cache)/KBYTE,eorun);
      printf("  [%lld,%lld] %d",Tdp+(vlow-cache)/KBYTE,Tdp+(vhgh-cache)/KBYTE,pdx);
      fflush(stdout);
#endif

      { int       freq, lcs, udx;
        int       asign, acont, adest;
        IOBuffer *ou;
        uint8    *l, *vcp, *jptr, *btop;
        int       m, n, k, b;
        
        freq = 0;
        vcp = vlcp[plen];
        if (vcp <= vlow)
          {
#ifdef DEBUG_MERGE
            printf("   vlow <= vcp\n");
            fflush(stdout);
#endif
            goto empty;
          }
      
        for (l = rend-KBYTE; l >= vcp; l -= KBYTE)
          { freq += l[CBYTE];
            if (freq >= FREQ)
              { vlow = l;
#ifdef DEBUG_MERGE
                printf("   %d vlow = %lld\n",freq,Tdp+(l-cache)/KBYTE);
                fflush(stdout);
#endif
                goto empty;
              }
          }
        lcs = freq;
        l   = rend;
        if ( ! eorun)
          { udx = cdx;
            l = rend;
            freq += l[CBYTE];
            if (freq >= FREQ)
              {
#ifdef DEBUG_MERGE
                printf("   %d too high at %lld\n",freq,Tdp+(l-cache)/KBYTE);
                fflush(stdout);
#endif
                goto empty;
              }
            ADVANCE(l)
            while (l[LBYTE] >= plen)
              { freq += l[CBYTE];
                if (freq >= FREQ)
                  {
#ifdef DEBUG_MERGE
                    printf("   %d too high at %lld\n",freq,Tdp+(l-cache)/KBYTE);
                    fflush(stdout);
#endif
                    cdx = udx;
                    goto empty;
                  }
                ADVANCE(l)
              }
            cdx = udx;
          }
#ifdef DEBUG_MERGE
        printf("    [%lld,%lld) w %d posts\n",Tdp+(vcp-cache)/KBYTE,Tdp+(l-cache)/KBYTE,freq);
        fflush(stdout);
#endif

        if (cdx >= lcs)
          b = cdx-lcs;
        else
          b = (cdx+POST_BUF_LEN) - lcs;
        if (b + freq > POST_BUF_LEN)
          { m = (b+freq) & POST_BUF_MASK;
            for (m--; m >= 0; m--)
              post[POST_BUF_LEN+m] = post[m];
          }

        nhits += suf1[CBYTE] * freq;
        g1len += suf1[CBYTE];
        tseed += suf1[CBYTE] * freq * plen;

        for (n = suf1[CBYTE]; n > 0; n--)
          { Current_Post(P1,aptr);
            asign = (aptr[ISIGN] & 0x80);
            aptr[ISIGN] &= 0x7f;
            acont = (apost >> ESHIFT);
            adest = Select[acont];
            jptr  = (uint8 *) (post+b);
            for (k = 0; k < freq; k++)
              { if (asign == (jptr[JSIGN] & 0x80))
                  ou = nunit + adest;
                else
                  ou = cunit + adest;
                btop = ou->btop;
                *btop++ = plen;
                memcpy(btop,aptr,IBYTE);
                btop += IBYTE;
                memcpy(btop,jptr,JBYTE);
                btop += JBYTE;

                ou->buck[acont] += 1;

#ifdef DEBUG_MERGE
                if (n == suf1[CBYTE])
                  { int64 ip;
                    int   ss;

                    ss = (jptr[JSIGN] & 0x80);
                    jptr[JSIGN] &= 0x7f;
                    printf("      %ld: %c",((int64 *) jptr)-post,ss?'-':'+');
                    ip = 0;
                    memcpy((uint8 *) (&ip),jptr+JPOST,JCONT);
                    printf(" %4lld",ip);
                    ip = 0;
                    memcpy((uint8 *) (&ip),jptr,JPOST);
                    printf(" %9lld\n",ip);
                    fflush(stdout);
                    if (ss)
                      jptr[JSIGN] |= 0x80;
                  }
#endif

                if (btop >= ou->bend)
                  { write(ou->file,ou->bufr,btop-ou->bufr);
                    ou->btop = ou->bufr;
                  }
                else
                  ou->btop = btop;

                jptr += sizeof(int64);
              }
#ifdef DEBUG_MERGE
            { int64 ip;

              if (n == suf1[CBYTE])
                printf("   vs\n");
              printf("      %lld: %c",P1->cidx,asign?'-':'+');
              ip = 0;
              memcpy((uint8 *) (&ip),aptr+IPOST,ICONT);
              printf(" %4lld",ip);
              ip = 0;
              memcpy((uint8 *) (&ip),aptr,IPOST);
              printf(" %9lld\n",ip);
              fflush(stdout);
            }
#endif
            Next_Post_Entry(P1);
          }

        Next_Kmer_Entry(T1);
        continue;
      }

    empty:
      JumpTo_Post_Index(P1,(int64) suf1[CBYTE]);
      Next_Kmer_Entry(T1);
    }

  { int j;

    for (j = 0; j < NPARTS; j++)
      { if (nunit[j].btop > nunit[j].bufr)
          write(nunit[j].file,nunit[j].bufr,nunit[j].btop-nunit[j].bufr);
        close(nunit[j].file);
        if (cunit[j].btop > cunit[j].bufr)
          write(cunit[j].file,cunit[j].bufr,cunit[j].btop-cunit[j].bufr);
        close(cunit[j].file);
      }
  }

  parm->nhits = nhits;
  parm->g1len = g1len;
  parm->tseed = tseed;
  return (NULL);
}
  

static void adaptamer_merge(char *g1,        char *g2,
                            Kmer_Stream *T1, Kmer_Stream *T2,
                            Post_List *P1,   Post_List *P2)
{ SP         parm[NTHREADS];
#ifndef DEBUG_MERGE
  pthread_t  threads[NTHREADS];
#endif
  uint8     *cache;
  int64      nhits, g1len, tseed;
  int        i, j;

  if (VERBOSE)
    { fprintf(stdout,"  Starting adaptive seed merge\n");
      fflush(stdout);
    }

  parm[0].T1 = T1;
  parm[0].T2 = T2;
  parm[0].P1 = P1;
  parm[0].P2 = P2;
  for (i = 1; i < NTHREADS; i++)
    { parm[i].T1 = Clone_Kmer_Stream(T1);
      parm[i].T2 = Clone_Kmer_Stream(T2);
      parm[i].P1 = Open_Post_List(g1);
      parm[i].P2 = Open_Post_List(g2);
    }

  cache = Malloc(NTHREADS*(P2->maxp+1)*KBYTE,"Allocating cache");
  if (cache == NULL)
    exit (1);

  for (i = 0; i < NTHREADS; i++)
    { IOBuffer *nu, *cu;

      parm[i].tid   = i;
      parm[i].cache = cache + i * (P2->maxp+1) * KBYTE;
      parm[i].nunit = nu = N_Units + i * NPARTS;
      parm[i].cunit = cu = C_Units + i * NPARTS;
      for (j = 0; j < NPARTS; j++)
        { nu[j].file = open(nu[j].name,O_WRONLY|O_CREAT|O_TRUNC,S_IRWXU);
          if (nu[j].file < 0)
            { fprintf(stderr,"%s: Cannot open %s for reading\n",Prog_Name,nu[j].name);
              exit (1);
            }
          cu[j].file = open(cu[j].name,O_WRONLY|O_CREAT|O_TRUNC,S_IRWXU);
          if (cu[j].file < 0)
            { fprintf(stderr,"%s: Cannot open %s for reading\n",Prog_Name,cu[j].name);
              exit (1);
            }
        }
      bzero(nu[0].buck,sizeof(int64)*NCONTS);
      bzero(cu[0].buck,sizeof(int64)*NCONTS);
    }

#ifdef DEBUG_MERGE
  for (i = 0; i < NTHREADS; i++)
    merge_thread(parm+i);
#else
  for (i = 1; i < NTHREADS; i++)
    pthread_create(threads+i,NULL,merge_thread,parm+i);
  merge_thread(parm);

  for (i = 1; i < NTHREADS; i++)
    pthread_join(threads[i],NULL);
#endif

  if (VERBOSE)
    { printf("\r    Completed 100%%\n");
      fflush(stdout);
    }
   
  free(cache);

  for (i = NTHREADS-1; i >= 1; i--)
    { Free_Kmer_Stream(parm[i].T1);
      Free_Kmer_Stream(parm[i].T2);
      Free_Post_List(parm[i].P1);
      Free_Post_List(parm[i].P2);
    }
  Free_Kmer_Stream(T1);
  Free_Kmer_Stream(T2);

  nhits = g1len = tseed = 0;
  for (i = 0; i < NTHREADS; i++)
    { nhits += parm[i].nhits;
      g1len += parm[i].g1len;
      tseed += parm[i].tseed;
    }

  if (VERBOSE)
    printf("\n  Total seeds = %lld, ave. len = %.1f, seeds per G1 position = %.1f\n\n",
           nhits,(1.*tseed)/nhits,(1.*nhits)/g1len);
}



/***********************************************************************************************
 *
 *   PAIR RE-IMPORT AND SORT
 *
 **********************************************************************************************/

typedef struct
  { int       in;
    int       swide;
    int       comp;
    DAZZ_DB  *DB1;
    DAZZ_DB  *DB2;
    int64    *buck;
    uint8    *buffer;
    uint8    *sarr;
    Range    *range;
  } RP;

static void *reimport_thread(void *args)
{ RP *parm = (RP *) args;
  int    swide  = parm->swide;
  int    in     = parm->in;
  int    comp   = parm->comp;
  uint8 *sarr   = parm->sarr;
  uint8 *bufr   = parm->buffer;
  int64 *buck   = parm->buck;

  DAZZ_READ *jreads = parm->DB2->reads;

  int64  ipost, jpost, icont, jcont, pdiag;
  uint8 *_ipost = (uint8 *) (&ipost);
  uint8 *_jpost = (uint8 *) (&jpost);
  uint8 *_icont = (uint8 *) (&icont);
  uint8 *_jcont = (uint8 *) (&jcont);
  uint8 *_pdiag  = (uint8 *) (&pdiag);

  uint8 *x;
  int    iolen, iunit, lcp, flip;
  int64  drem, flag, mask;
  uint8 *bend, *btop, *b;

  iolen = 2*NPARTS*1000000;
  iunit = IBYTE + JBYTE + 1;

  bend = bufr + read(in,bufr,iolen);

  if (bend-bufr < iolen)
    btop = bend;
  else
    btop = bend-iunit;
  b = bufr;

  ipost = 0;
  jpost = 0;
  jcont = 0;
  icont = 0;

  flag = (0x1ll << (8*JCONT-1));
  mask = flag-1;

  while (1)
    { lcp = *b++;
      memcpy(_ipost,b,IPOST);
      b += IPOST;
      memcpy(_icont,b,ICONT);
      b += ICONT;
      memcpy(_jpost,b,JPOST);
      b += JPOST;
      memcpy(_jcont,b,JCONT);
      b += JCONT;
      flip = ((jcont & flag) != 0);
      jcont &= mask;

      x = sarr + swide * buck[icont]++;
      *x++ = lcp;
      if (comp)
        drem = ipost + jpost;
      else
        { drem = (ipost - jpost) + jreads[Perm2[jcont]].rlen;
          if (flip)
            ipost += (KMER-lcp);
        }
      pdiag = (drem >> BUCK_SHIFT);
      *x++ = drem-(pdiag<<BUCK_SHIFT);

      memcpy(x,_ipost,IPOST);
      x += IPOST;
      memcpy(x,_pdiag,DBYTE);
      x += DBYTE;
      memcpy(x,_jcont,JCONT);
      x += JCONT;

      if (b >= btop)
        { int ex = bend-b;
          memcpy(bufr,b,ex);
          bend = bufr+ex;
          bend += read(in,bend,iolen-ex);
          if (bend == bufr)
            break;
          if (bend-bufr < iolen)
            btop = bend;
          else
            btop = bend-iunit;
          b = bufr;
        }
    }

  close(in);

  return (NULL);
}

void print_seeds(uint8 *sarray, int swide, Range *range, int64 *panel,
                 DAZZ_DB *DB1, DAZZ_DB *DB2, int comp)
{ uint8 *e, *x;
  int    n, p;
  int    lcp, drm;

  int64  ipost, dbuck, jcont;
  int64  jpost, diag;
  uint8 *_ipost = (uint8 *) (&ipost);
  uint8 *_dbuck = (uint8 *) (&dbuck);
  uint8 *_jcont = (uint8 *) (&jcont);

  ipost = 0;
  dbuck = 0;
  jcont = 0;

  (void) DB1;

  for (n = 0; n < NTHREADS; n++)
    { x = sarray + range[n].off;
      for (p = range[n].beg; p < range[n].end; p++)
        { e = x + panel[p];
          while (x < e)
            { lcp = *x++;
              drm = *x++;
              memcpy(_ipost,x,IPOST);
              x += IPOST;
              memcpy(_dbuck,x,DBYTE);
              x += DBYTE;
              memcpy(_jcont,x,JCONT);
              x += JCONT;

              if (comp)
                { diag  = (dbuck<<BUCK_SHIFT)+drm;
                  jpost = diag-ipost;
                }
              else
                { diag  = ((dbuck<<BUCK_SHIFT)+drm) - DB2->reads[Perm2[jcont]].rlen;
                  jpost = ipost-diag;
                }

             if (jpost < 0 || jpost > DB2->reads[Perm2[jcont]].rlen)
               printf("SHOUT OUT\n");

              printf("  %10ld:  %5d %5lld: %8lld  %10lld x %10lld  (%2d)  %2d\n",
                     (x-sarray)/swide,p,jcont,diag,ipost,jpost,drm,lcp);
            }
        }
    }
}

#ifdef PRINT_SEQ

static void print_seq(char *seq, int b, int e)
{ static int toA[5] = { 'a', 'c', 'g', 't', '*' };
  int j;

  for (j = b; j < e; j++)
    printf("%c",toA[(int) seq[j]]);
  printf("\n");
}

#endif

static int entwine(Path *jpath, uint8 *jtrace, Path *kpath, uint8 *ktrace, int *where, int show)
{ int   ac, b2, y2, yp, ae;
  int   i, j, k;
  int   num, den, min;

#ifdef DEBUG_ENTWINE
  if (show)
    printf("\n");
#endif

  *where = -1;

  y2 = jpath->bbpos;
  b2 = kpath->bbpos;
  j  = jpath->abpos/TSPACE;
  k  = kpath->abpos/TSPACE;

  ac = k*TSPACE;

  j = 1 + 2*(k-j);
  k = 1;
  for (i = 1; i < j; i += 2)
    y2 += jtrace[i];

  if (j == 1)
    yp = y2 + (jtrace[j] * (kpath->abpos - jpath->abpos)) / (ac+TSPACE - jpath->abpos);
  else
    yp = y2 + (jtrace[j] * (kpath->abpos - ac)) / TSPACE;

#ifdef DEBUG_ENTWINE
  if (show)
    printf("   @ %5d : %5d %5d = %4d\n",kpath->abpos,yp,b2,b2-yp);
#endif

  num = b2-yp;
  den = 1;
  min = num;

  ae = jpath->aepos;
  if (ae > kpath->aepos)
    ae = kpath->aepos;

  for (ac += TSPACE; ac < ae; ac += TSPACE)
    { y2 += jtrace[j];
      b2 += ktrace[k];
      j += 2;
      k += 2;

#ifdef DEBUG_ENTWINE
      if (show)
        printf("   @ %5d : %5d %5d = %4d\n",ac,y2,b2,b2-y2);
#endif

      i = b2-y2;
      num += i;
      den += 1;
      if (min < 0 && min < i)
        { if (i >= 0)
            min = 0; 
          else
            min = i;
        }
      else if (min > 0 && min > i)
        { if (i <= 0)
            min = 0;
          else
            min = i;
        }
      if (i == 0)
        *where = ac;
    }

  ac -= TSPACE;
  if (ae == jpath->aepos)
    { y2 = jpath->bepos;
      if (kpath->aepos >= ac)
        b2 += (ktrace[k] * (ae - ac)) / TSPACE;
      else
        b2 += (ktrace[k] * (ae - ac)) / (kpath->aepos - ac);
    }
  else
    { b2 = kpath->bepos;
      if (jpath->aepos >= ac)
        y2 += (jtrace[j] * (ae - ac)) / TSPACE;
      else
        y2 += (jtrace[j] * (ae - ac)) / (jpath->aepos - ac);
    }

#ifdef DEBUG_ENTWINE
  if (show)
    printf("   @ %5d : %5d %5d = %4d\n",ae,y2,b2,b2-y2);
#endif

  i = b2-y2;
  num += i;
  den += 1;
  if (min < 0 && min < i)
    { if (i >= 0)
        min = 0; 
      else
        min = i;
    }
  else if (min > 0 && min > i)
    { if (i <= 0)
        min = 0;
      else
        min = i;
    }

  if (show)
    { printf("MINIM = %d AVERAGE = %d",min,num/den);
      if (*where >= 0)
        printf(" WHERE = %d",*where);
      printf("\n");
    }
#ifdef DEBUG_ENTWINE
#endif

  return (min);
}

typedef struct
  { int  jpost;
    int  lcp;
  } Jspan;

typedef struct

  { DAZZ_DB    *DB1, *DB2;
    int         lmax;           //  dynamic list for adjacent bucket merges
    Jspan      *list;
    FILE       *ofile;
    FILE       *tfile;
    int64       nhits;
    int64       nlass;
    int64       nlive;
    int64       nlcov;
                            //  See align.h for doc on the following:
    Work_Data  *work;           //  work storage for alignment module
    Align_Spec *spec;           //  alignment spec
    Alignment   align;          //  alignment record
    Overlap     ovl;            //  overlap record

  } Contig_Bundle;

#define SHOW_LAS(printer)					\
  if (comp)							\
    { Complement_Seq(align->aseq+(alen-path->aepos),		\
                     path->aepos-path->abpos);			\
      Complement_Seq(align->bseq+(blen-path->bepos),		\
                     path->bepos-path->bbpos);			\
    }								\
  Compute_Trace_PTS(align,work,TSPACE,GREEDIEST);		\
  printer(stdout,align,work,4,100,10,0,8);			\
  fflush(stdout);						\
  if (comp)							\
    { Complement_Seq(align->aseq+(alen-path->aepos),		\
                     path->aepos-path->abpos);			\
      Complement_Seq(align->bseq+(blen-path->bepos),		\
                     path->bepos-path->bbpos);			\
    }

static int JSORT(const void *l, const void *r)
{ Jspan *x = (Jspan *) l;
  Jspan *y = (Jspan *) r;

  return (x->jpost-y->jpost);
}

static int ALN_SORT(void *iblock, const void *l, const void *r)
{ int64 x = *((int64 *) l);
  int64 y = *((int64 *) r);
  Overlap *ol, *or;

  ol = (Overlap *) (iblock+x);
  or = (Overlap *) (iblock+y);
  return (ol->path.abpos - or->path.abpos);
}

//  [beg,end) in the sorted array of width swide elements contain all the adaptive seeds between
//    the contigs in the parameter pair.  Look for seed chains in each pair of diagaonl buckets
//    of sufficient score, and when found search for an alignment, outputing it if found.

void align_contigs(uint8 *beg, uint8 *end, int swide, int ctg1, int ctg2, Contig_Bundle *pair)
{ int    lmax = pair->lmax;
  Jspan *list = pair->list;

  Overlap    *ovl   = &(pair->ovl);
  int         comp  = (ovl->flags != 0);
#ifdef CALL_ALIGNER
  Work_Data  *work  = pair->work;
  Align_Spec *spec  = pair->spec;
  Alignment  *align = &(pair->align);
  Path       *path  = align->path;
#ifndef DEBUG_ALIGN
  FILE       *ofile = pair->ofile;
  FILE       *tfile = pair->tfile;
#endif
#endif
#if defined(DEBUG_SEARCH) || defined(DEBUG_HIT)
  int         repgo;
#endif

  uint8 *b, *m, *e;

  int64  nhit, nlas, nmem, nliv, ncov;
  int64  alen, blen;
  int64  aoffset, doffset;

  int    new, aux;
  int64  ndiag, cdiag;
  uint8 *_ndiag = (uint8 *) (&ndiag);

  int64  ipost, apost;
  uint8 *_ipost = (uint8 *) (&ipost);
  uint8 *_apost = (uint8 *) (&apost);

  ndiag = 0;
  ipost = 0;
  apost = 0;

  ctg1 = Perm1[ctg1];
  ctg2 = Perm2[ctg2];

  blen   = pair->DB2->reads[ctg2].rlen;
  alen   = pair->DB1->reads[ctg1].rlen;
  nhit   = 0;
  nlas   = 0;
  nmem   = 0;
  nliv   = 0;
  ncov   = 0;

  aoffset = alen-KMER;

  //  Find segments b,m,e such that b > m and diag is cdiag for elements [b,m) and
  //    cdiag+1 for elements [m,e) (if m < e)
  //  If m == e (i.e. !aux) and b,m = m',e' of previous find (i.e. !new) then don't examine
  //    as the chain for this triple is subset of the chain for the previous triple.

  b = e = beg + (IPOST+2);
  memcpy(_ndiag,e,DBYTE);
  cdiag = ndiag;
  while (ndiag == cdiag && e < end)
    { e += swide;
      memcpy(_ndiag,e,DBYTE);
    }

#if defined(DEBUG_SEARCH) || defined(DEBUG_HIT)
  repgo = (ctg1 == -1) && (ctg2 == -1);
  if (repgo)
    printf("\n  Contig %d vs Contig %d\n",ctg1,ctg2);
#endif

  while (1)
    { m = e;
      aux = 0;
      while (ndiag == cdiag+1 && e < end)
        { e += swide;
          memcpy(_ndiag,e,DBYTE);
          aux = 1;
        }

      if (new || aux)
        { int    go, lcp, wch, mix;
          int64  lps, cov, npost, alast;
          int    dgmin, dgmax, dg;
          int    apmin, apmax;
          uint8 *s, *t;
          int    len;
     
#ifdef DEBUG_SEARCH
          if (repgo)
            { printf("Diag %lld",cdiag);
              if (aux)
                printf("+1");
              printf("\n");
            }
#endif

          //  Have triple b,m,e, b > m, to examine.  Capture the ipost-ordered merge of [b,m) and
          //    [m,e) in list[0..len) and process any above-threshold chains encountered while
          //    doing the merge.

          if (comp)
            { doffset = aoffset-(cdiag<<BUCK_SHIFT);
              alast   = alen+1;
            }
          else
            { doffset = (cdiag<<BUCK_SHIFT)-blen;
              alast   = -1;
            }

          if (e-b > lmax)
            { lmax = (e-b)*1.2+100;
              list = Realloc(list,(lmax+1)*sizeof(Jspan),"Expanding merge index");
              if (list == NULL)
                exit (1);
            }

          e -= IPOST;
          m -= IPOST;

          s = b-IPOST;
          memcpy(_ipost,s,IPOST);
          t = m; 
          if (aux)
            memcpy(_apost,t,IPOST);
          else
            apost = MAX_INT64;

          lps = -CHAIN_BREAK;
          cov = 0;
          go  = 1;
          while (go)
            { if (apost < ipost)
                { lcp   = t[-2];
                  dg    = t[-1] + BUCK_WIDTH;
                  npost = apost;
                  t += swide;
                  if (t >= e)
                    apost = MAX_INT64;
                  else
                    memcpy(_apost,t,IPOST);
                  wch = 0x2;
                }
              else
                { lcp   = s[-2];
                  dg    = s[-1];
                  npost = ipost;
                  s += swide;
                  if (s >= m)
                    { if (s > m)
                        go = 0;
                      else
                        ipost = MAX_INT64;
                    }
                  else
                    memcpy(_ipost,s,IPOST);
                  wch = 0x1;
                }
  
              if (npost < lps + CHAIN_BREAK)
                { int64 cps;

                  cps = npost + lcp;
                  if (cps > lps)
                    { if (npost >= lps)
                        cov += lcp;
                      else
                        cov += cps-lps;
                      lps = cps;
                    }
                  list[len].jpost = npost-dg;
                  list[len++].lcp = lcp;
                  mix |= wch;
                  if (dg < dgmin)
                    dgmin = dg;
                  else if (dg > dgmax)
                    dgmax = dg;
                }
              else
                { if (cov >= CHAIN_MIN && (mix != 1 || new))

                    //  Have a chain that covers CHAIN_MIN or more bases of the A-sequence:

                    { int    k, mo;
                      int64  jcov;
                      int64  anti;

                      nhit += 1;
#ifdef DEBUG_SEARCH
                      if (repgo)
                        printf("                  Process\n");
#endif
#ifdef DEBUG_HIT
                      if (repgo)
                        { printf("Hit on bucket %lld",cdiag);
                          if (aux)
                            printf("+1");
                          printf(" A-cov = %lld # seeds = %d\n",cov,len);
                        }
#endif

                      //  Check that also cover CHAIN_MIN or more bases of the B-sequence
                      //    and involves hit in new diagonal (if aux then + else .)

                      { int64 jlps, jpost;
                        int64 jcps, jlcp;

                        qsort(list,len,sizeof(Jspan),JSORT);

                        jlps  = -128;
                        jcov  = 0;
                        for (k = 0; k < len; k++)
                          { jlcp  = list[k].lcp;
                            jpost = list[k].jpost;
                            jcps  = jpost + jlcp;
                            if (jcps > jlps)
                              { if (jpost >= jlps)
                                  jcov += jlcp;
                                else
                                  jcov += jcps-jlps;
                                jlps = jcps;
                              }
                          }
#ifdef DEBUG_HIT
                        if (repgo)
                          printf("  J-coverage = %lld\n",jcov);
#endif
                      }

		      if (jcov >= CHAIN_MIN)
                        {

                          //  Filter passed, now search for local alignments for each
                          //    reduced seed match (several adaptemer seeds that overlap)

                          //  Have a hit in the trapezoid (apmin..apmax,dgmin..dgmax)
                          //  Search for a local alignment spanning it a-center

                          apmax = lps;

                          //  Fetch contig sequences if not already loaded
#ifdef CALL_ALIGNER
                          if (ctg1 != ovl->aread)
                            { Load_Read(pair->DB1,ctg1,align->aseq,0);
                              align->alen = alen;
                              ovl->aread = ctg1;
                              if (comp)
                                Complement_Seq(align->aseq,align->alen);
#ifdef DEBUG_HIT
                              if (repgo)
                                printf("Loading A = %d%c\n",ctg1,comp?'c':'n');
                              fflush(stdout);
#endif
                            }
                          if (ctg2 != ovl->bread)
                            { Load_Read(pair->DB2,ctg2,align->bseq,0);
                              align->blen = blen;
                              ovl->bread = ctg2;
#ifdef DEBUG_HIT
                              if (repgo)
                                printf("Loading B = %dn\n",ctg2);
                              fflush(stdout);
#endif
                            }
#endif

#ifdef DEBUG_HIT
                          if (repgo)
                            { if (comp)
                                printf("  Box:  %10lld,%10lld: %4d %3d\n",
                                       aoffset-apmax,doffset-dgmax,apmax-apmin,dgmax-dgmin);
                              else
                                printf("  Box:  %10d,%10lld: %4d %3d [%d %lld]\n",
                                       apmin,dgmin+doffset,apmax-apmin,dgmax-dgmin,apmax,lps);
                              fflush(stdout);
                            }
#endif
                          if (comp)
                            { if ((mo = (apmax <= alast)))
                                { anti  = doffset - dgmin;
                                  dgmin = doffset - dgmax;
                                  dgmax = anti;
                                  anti  = ((aoffset << 1) - (apmin + apmax)) - ((dgmax+dgmin)>>1);
                                }
                            }
                          else
                            { if ((mo = (apmin >= alast)))
                                { dgmin += doffset;
                                  dgmax += doffset;
                                  anti   = (apmin + apmax) - ((dgmax+dgmin)>>1);
                                }
                            }
#ifdef DEBUG_HIT
                          if (mo & repgo)
                            printf("      Diag = %d:%d  Anti = %lld\n",
                                   dgmin,dgmax,anti);
#endif
#ifdef CALL_ALIGNER
                          if (mo)
                            { Local_Alignment(align,work,spec,dgmin,dgmax,anti,-1,-1);
#ifdef DEBUG_ALIGN
                              if (path->aepos - path->abpos >= ALIGN_MIN)
                                { printf("Local %d: %d-%d vs %d %d\n",k-lst,
                                         path->abpos,path->aepos,path->bbpos,path->bepos);
                                  SHOW_LAS(Print_Alignment)
                                  nlas += 1;
                                }
                              else
                                printf("Not found, len = %d\n",
                                       path->aepos-path->abpos);
#else
                              if (path->aepos - path->abpos >= ALIGN_MIN)
                                { if (ABYTE)
                                    Compress_TraceTo8(ovl,0);
                                  if (Write_Overlap(tfile,ovl,TBYTES))
                                    { fprintf(stderr,"%s: Cannot write output\n",Prog_Name);
                                      exit (1);
                                    }
                                  nlas += 1;
                                  nmem += path->tlen * TBYTES + OVL_SIZE;
                                }
#endif
                              if (comp)
                                alast = alen - path->abpos;
                              else
                                alast = path->aepos;
                            }
#ifdef DEBUG_HIT
                          else if (repgo)
                            printf("BLOCKED %lld\n",alast);
#endif
#endif // CALL_ALIGNER
                        }
                    }
#ifdef DEBUG_SEARCH
                  else if (repgo)
                    printf("                  Break\n");
#endif

                  if (go)
                    { cov = lcp;
                      lps = npost + lcp;
                      mix = wch;
                      len = 0;
                      dgmin = dgmax = dg;
                      apmin = npost;
                      list[len].jpost = npost-dg;
                      list[len++].lcp = lcp;
                    }
                }

#ifdef DEBUG_SEARCH
              if (go && repgo)
                { uint8 *n;

                  dg += (cdiag<<BUCK_SHIFT);
                  if (wch == 0x1)
                    n = s - swide;
                  else
                    n = t - swide;
                  if (comp)
                    printf("   %c %10ld: c %10lld x %10lld %2d %4lld (%d)\n",
                           wch==0x1?'.':'+',(n-beg)/swide,npost,dg-npost,n[-2],cov,n[-1]);
                  else
                    printf("   %c %10ld: n %10lld x %10lld %2d %4lld (%d)\n",
                           wch==0x1?'.':'+',(n-beg)/swide,npost,npost-(dg-blen),n[-2],cov,n[-1]);
                }
#endif
            }

          e += IPOST;
          m += IPOST;

          ipost = apost = 0;
        }

      if (e >= end) break;

      if (aux)
        { b = m;
          cdiag += 1;
          new = 0;
        }
      else
        { b = e;
          cdiag = ndiag;
          while (ndiag == cdiag && e < end)
            { e += swide;
              memcpy(_ndiag,e,DBYTE);
            }
          new = 1;
        }
    }

  //  Detect and remove redundant alignments

  if (nlas > 0)
    { void    *oblock = Malloc(nmem,"Allocating overlap block");
      int64   *perm   = Malloc(nlas*sizeof(int64),"Allocating permutation array");
      int      j, k, where, dist;
      Path     tpath;
      void    *tcopy;

      rewind(tfile);
      if (fread(oblock,nmem,1,tfile) != 1) 
        { fprintf(stderr,"\n%s: Cannot read overlap block file\n",Prog_Name);
          exit (1);
        }


      { int64 off;

        off = -PTR_SIZE;
        for (j = 0; j < nlas; j++)
          { perm[j] = off;
            off += OVL_SIZE + ((Overlap *) (oblock+off))->path.tlen*TBYTES;
          }
      }

      qsort_r(perm,nlas,sizeof(int64),oblock,ALN_SORT);

#define ELIMINATED  0x4

      for (j = nlas-1; j >= 0; j--)
        { Overlap *o  = (Overlap *) (oblock+perm[j]);
          Path    *op = &(o->path);

          for (k = j+1; k < nlas; k++)
            { Overlap *w  = (Overlap *) (oblock+perm[k]);
              Path    *wp = &(w->path);

              if (op->aepos <= wp->abpos)
                break;
              if (w->flags & ELIMINATED)
                continue;

              if (op->abpos == wp->abpos && op->bbpos == wp->bbpos)
                if (op->aepos == wp->aepos && op->bepos == wp->bepos)
                  { if (op->diffs < wp->aepos)
                      { w->flags |= ELIMINATED;
                        continue;
                      }
                    else
                      { o->flags |= ELIMINATED;
                        break;
                      }
                  }
                else
                  { if (op->aepos > wp->aepos)
                      { w->flags |= ELIMINATED;
                        // printf("  START - %d %d\n",op->aepos-wp->aepos,op->diffs-wp->diffs);
                        continue;
                      }
                    else
                      { o->flags |= ELIMINATED;
                        // printf("  START . %d %d\n",wp->aepos-op->aepos,wp->diffs-op->diffs);
                        break;
                      }
                  }
              else
                if (op->aepos == wp->aepos && op->bepos == wp->bepos)
                  { if (op->abpos < wp->abpos)
                      { w->flags |= ELIMINATED;
                        // printf("  END - %d %d\n",wp->abpos-op->abpos,op->diffs-wp->diffs);
                        continue;
                      }
                    else
                      { o->flags |= ELIMINATED;
                        // printf("  END . %d %d\n",op->abpos-wp->abpos,wp->diffs-op->diffs);
                        break;
                      }
                  }
            }
        }

      for (j = nlas-1; j >= 0; j--)
        { Overlap *o  = (Overlap *) (oblock+perm[j]);
          Path    *op = &(o->path);

          if (o->flags & ELIMINATED)
            continue;

          for (k = j+1; k < nlas; k++)
            { Overlap *w  = (Overlap *) (oblock+perm[k]);
              Path    *wp = &(w->path);

              if (op->aepos <= wp->abpos)
                break;
              if (w->flags & ELIMINATED)
                continue;
              if (op->bepos <= wp->bbpos || op->bbpos >= wp->bepos)
                continue;

              dist = entwine(&(o->path),(uint8 *) (o+1),&(w->path),(uint8 *) (w+1),&where,0);
              if (where != -1)
                { //  Fuse here
// printf("FUSE %d %d\n",op->aepos-op->abpos,wp->aepos-wp->abpos);
                  // printf(" %3d: %d-%d vs %d-%d\n            %d-%d vs %d-%d\n",
                         // where,o->path.abpos,o->path.aepos,w->path.abpos,w->path.aepos,
                         // o->path.bbpos,o->path.bepos,w->path.bbpos,w->path.bepos);
                  continue;
                }
              if (dist < 0 && wp->bepos <= op->bepos+10)
                { w->flags |= ELIMINATED;
                  continue;
                }
              if (dist > 0 && wp->abpos <= op->abpos+10 && wp->bepos+10 >= op->bepos)
                { o->flags |= ELIMINATED;
                  break;
                }
continue;

// printf("OTHER\n");
              // printf(" %3d x %3d: %d-%d vs %d-%d\n            %d-%d vs %d-%d\n",
                     // j,k,o->path.abpos,o->path.aepos,w->path.abpos,w->path.aepos,
                     // o->path.bbpos,o->path.bepos,w->path.bbpos,w->path.bepos);
              dist = entwine(&(o->path),(uint8 *) (o+1),&(w->path),(uint8 *) (w+1),&where,1);

              if (op->abpos <= wp->abpos && op->aepos >= wp->aepos)
                { w->flags |= ELIMINATED;
                  printf("  CONTAIN - %d %d\n",
                         (wp->abpos-op->abpos)+(op->aepos-wp->aepos),op->diffs-wp->diffs);
                  // continue;
                }
              if (wp->abpos <= op->abpos && wp->aepos >= op->aepos)
                { o->flags |= ELIMINATED;
                  printf("  CONTAIN . %d %d\n",
                         (op->abpos-wp->abpos)+(wp->aepos-op->aepos),wp->diffs-op->diffs);
                  // break;
                }

              printf("\nAlign 1\n");
              tpath = o->path;
              align->path = &tpath;
              tpath.trace = tcopy = Malloc(sizeof(uint16)*tpath.tlen,"Trace");
              memcpy(tcopy,o+1,tpath.tlen);
              { uint16 *t16 = (uint16 *) tcopy;
                uint8  *t8  = (uint8  *) tcopy;
                int     nn;

                for (nn = tpath.tlen-1; nn >= 0; nn--)
                  t16[nn] = t8[nn];
              }
              Compute_Trace_PTS(align,work,TSPACE,GREEDIEST);
              Print_Reference(stdout,align,work,4,100,10,0,8);
              fflush(stdout);
              free(tcopy);

              printf("\nAlign 2\n");
              tpath = w->path;
              align->path = &tpath;
              tpath.trace = tcopy = Malloc(sizeof(uint16)*tpath.tlen,"Trace");
              memcpy(tcopy,w+1,tpath.tlen);
              { uint16 *t16 = (uint16 *) tcopy;
                uint8  *t8  = (uint8  *) tcopy;
                int     nn;

                for (nn = tpath.tlen-1; nn >= 0; nn--)
                  t16[nn] = t8[nn];
              }
              Compute_Trace_PTS(align,work,TSPACE,GREEDIEST);
              Print_Reference(stdout,align,work,4,100,10,0,8);
              fflush(stdout);
              free(tcopy);

              align->path = &(ovl->path);
	    }
        }

      for (j = 0; j < nlas; j++)
        { Overlap *o = (Overlap *) (oblock+perm[j]);

          if (o->flags & ELIMINATED)
            continue;
          fwrite( ((char *) o)+PTR_SIZE, OVL_SIZE, 1, ofile);
          fwrite( (char *) (o+1), TBYTES, o->path.tlen, ofile);
          nliv += 1;
          ncov += o->path.aepos - o->path.abpos;
        }

      rewind (tfile);
      free(perm);
      free(oblock);
    }

  pair->lmax   = lmax;
  pair->list   = list;
  pair->nhits += nhit;
  pair->nlass += nlas;
  pair->nlive += nliv;
  pair->nlcov += ncov;
}


typedef struct
  { int       swide;
    int       comp;
    int64    *panel;
    uint8    *sarr;
    Range    *range;
    DAZZ_DB   DB1;
    DAZZ_DB   DB2;
    FILE     *ofile;
    FILE     *tfile;
    int64     nhits;
    int64     nlass;
    int64     nlive;
    int64     nlcov;
  } TP;

static void *search_seeds(void *args)
{ TP *parm = (TP *) args;
  int      swide  = parm->swide;
  int      comp   = parm->comp;
  int64   *panel  = parm->panel;
  uint8   *sarray = parm->sarr;
  Range   *range  = parm->range;
  int      beg    = range->beg;
  int      end    = range->end;
  DAZZ_DB *DB1    = &(parm->DB1);
  DAZZ_DB *DB2    = &(parm->DB2);
  int      foffs  = swide-JCONT;
  FILE    *ofile  = parm->ofile;
  FILE    *tfile  = parm->tfile;

  int    icrnt;
  int64  jcrnt;
  uint8 *_jcrnt = (uint8 *) (&jcrnt);

  Contig_Bundle _pair, *pair = &_pair;

  uint8 *x, *e, *b;

  jcrnt = 0;

  pair->lmax = 1000;
  pair->list = Malloc(pair->lmax*sizeof(Jspan),"Allocating merge index");
  if (pair->list == NULL)
    exit (1);
  pair->DB1 = DB1;
  pair->DB2 = DB2;
  pair->align.aseq = New_Read_Buffer(DB1);
  pair->align.bseq = New_Read_Buffer(DB2);
  pair->align.path = &(pair->ovl.path);
  if (comp)
    { pair->ovl.flags   = COMP_FLAG;
      pair->align.flags = ACOMP_FLAG;
    }
  else
    { pair->ovl.flags   = 0;
      pair->align.flags = 0;
    }
  pair->ovl.aread = -1;
  pair->ovl.bread = -1;
  pair->work = New_Work_Data();
  pair->spec = New_Align_Spec(ALIGN_RATE,100,DB1->freq,0);
  pair->ofile = ofile;
  pair->tfile = tfile;
  pair->nhits = 0;
  pair->nlass = 0;
  pair->nlive = 0;
  pair->nlcov = 0;

  x = sarray + range->off;
  for (icrnt = beg; icrnt < end; icrnt++)
    { e = x + panel[icrnt];

      memcpy(_jcrnt,x+foffs,JCONT);
      b = x;
      for (x += swide; x < e; x += swide)
        if (memcmp(_jcrnt,x+foffs,JCONT))
          { align_contigs(b,x,swide,icrnt,(int) jcrnt,pair);
            memcpy(_jcrnt,x+foffs,JCONT);
            b = x;
          }
      align_contigs(b,x,swide,icrnt,jcrnt,pair);
    }

  Free_Align_Spec(pair->spec);
  Free_Work_Data(pair->work);
  free(pair->align.aseq-1);
  free(pair->align.bseq-1);
  free(pair->list);

  parm->nhits += pair->nhits;
  parm->nlass += pair->nlass;
  parm->nlive += pair->nlive;
  parm->nlcov += pair->nlcov;
  return (NULL);
}

static void pair_sort_search(DAZZ_DB *DB1, DAZZ_DB *DB2)
{ uint8 *sarray;
  int    swide;
  int64  nels;

  RP     rarm[NTHREADS];
  TP     tarm[NTHREADS];
#ifndef DEBUG_SORT
  pthread_t threads[NTHREADS];
#endif
  int64    *panel;
  Range     range[NTHREADS];

  IOBuffer *unit[2], *nu;
  int       nused;
  int       i, p, j, u;

  if (VERBOSE)
    { fprintf(stdout,"  Starting seed sort and alignment search, %d parts\n",2*NPARTS);
      fflush(stdout);
    }

  unit[0] = N_Units;
  unit[1] = C_Units;

  { int64 cum, nelmax;

    nelmax = 0;
    for (u = 0; u < 2; u++)
      { cum = 0;
        nu = unit[u];

        for (j = 0; j < NCONTS; j++)
          { for (i = 0; i < NTHREADS; i++)
              { cum += nu[i].buck[j];
                nu[i].buck[j] = cum;
              }
            if (j+1 == NCONTS || Select[j] != Select[j+1])
              { if (cum > nelmax)
                  nelmax = cum;
                cum = 0;
              }
          }

        for (j = NCONTS-1; j >= 0; j--)
          { for (i = NTHREADS-1; i >= 1; i--)
              nu[i].buck[j] = nu[i-1].buck[j];
            if (j == 0 || Select[j] != Select[j-1])
              nu[0].buck[j] = 0;
            else
              nu[0].buck[j] = nu[NTHREADS-1].buck[j-1];
          }
      }

    swide  = IPOST + DBYTE + JCONT + 2;
    sarray = Malloc((nelmax+1)*swide,"Sort Array");
    panel  = Malloc(NCONTS*sizeof(int64),"Bucket Array");
    if (sarray == NULL || panel == NULL)
      exit (1);
  }

  for (p = 0; p < NTHREADS; p++)
    { rarm[p].swide  = swide;
      rarm[p].sarr   = sarray;
      rarm[p].buffer = N_Units[p].bufr;   //  NB: Units have been transposed
      rarm[p].range  = range+p;
      rarm[p].DB1    = DB1;
      rarm[p].DB2    = DB2;

      tarm[p].swide  = swide;
      tarm[p].sarr   = sarray;
      tarm[p].panel  = panel;
      tarm[p].range  = range+p;

      tarm[p].DB1    = *DB1;
      tarm[p].DB2    = *DB2;
      if (p > 0)
        { tarm[p].DB1.bases = fopen(Catenate(DB1->path,"","",".bps"),"r");
          if (tarm[p].DB1.bases == NULL)
            { fprintf(stderr,"%s: Cannot open another copy of DB\n",Prog_Name);
              exit (1);
            }
          tarm[p].DB2.bases = fopen(Catenate(DB2->path,"","",".bps"),"r");
          if (tarm[p].DB2.bases == NULL)
            { fprintf(stderr,"%s: Cannot open another copy of DB\n",Prog_Name);
              exit (1);
            }
        }

      tarm[p].nhits = 0;
      tarm[p].nlass = 0;
      tarm[p].nlive = 0;
      tarm[p].nlcov = 0;

      tarm[p].ofile = fopen(Catenate(SORT_PATH,"/",ALGN_UNIQ,Numbered_Suffix(".",p,".las")),"w");
      if (tarm[p].ofile == NULL)
        { fprintf(stderr,"%s: Cannot open %s/%s.%d.las for writing\n",
                         Prog_Name,SORT_PATH,ALGN_UNIQ,p);
          exit (1);
        }
      fwrite(&nels,sizeof(int64),1,tarm[p].ofile);
      nused = 100;
      fwrite(&nused,sizeof(int),1,tarm[p].ofile);

      tarm[p].tfile = fopen(Catenate(SORT_PATH,"/",ALGN_PAIR,Numbered_Suffix(".",p,".las")),"w+");
      if (tarm[p].tfile == NULL)
        { fprintf(stderr,"%s: Cannot open %s/%s.%d.las for reading & writing\n",
                         Prog_Name,SORT_PATH,ALGN_PAIR,p);
          exit (1);
        }
    }

  for (u = 0; u < 2; u++)
   for (i = 0; i < NPARTS; i++)
    { nu = unit[u] + i*NTHREADS;

      if (VERBOSE)
        { fprintf(stdout,"\r    Loading seeds for part %d  ",u*NPARTS+i+1);
          fflush(stdout);
        }

      for (p = 0; p < NTHREADS; p++)
        { rarm[p].in = open(nu[p].name,O_RDONLY);
          if (rarm[p].in < 0)
            { fprintf(stderr,"%s: Cannot open %s for reading\n",Prog_Name,nu[p].name);
              exit (1);
            }
          rarm[p].buck = nu[p].buck;
          rarm[p].comp = u;
        }

#ifdef DEBUG_SORT
      for (p = 0; p < NTHREADS; p++)
        reimport_thread(rarm+p);
#else
      for (p = 1; p < NTHREADS; p++)
        pthread_create(threads+p,NULL,reimport_thread,rarm+p);
      reimport_thread(rarm);
      for (p = 1; p < NTHREADS; p++)
        pthread_join(threads[p],NULL);
#endif

      for (p = 0; p < NTHREADS; p++)
        unlink(nu[p].name);

#ifdef DEBUG_SORT
      for (p = 0; p < NTHREADS; p++)
        printf("  %s",nu[p].name);
      printf("\n");
      for (j = 0; j < NCONTS; j++)
        { printf(" %4d:",j);
          for (p = 0; p < NTHREADS; p++)
            printf(" %10lld",unit[u][p].buck[j]);
          printf("\n");
        }
     fflush(stdout);
#endif

      { int64 prev, next;

        bzero(panel,sizeof(int64)*NCONTS);
        prev = 0;
        for (j = IDBsplit[i]; j < IDBsplit[i+1]; j++)
          { next = nu[NTHREADS-1].buck[j];
            panel[j] = (next - prev)*swide;
            prev = next;
          }
        nels = next;

#ifdef DEBUG_SORT
        for (p = 0; p < NCONTS; p++)
          if (panel[p] > 0)
            printf(" %2d(%2d): %10lld %10lld\n",p,Perm1[p],panel[p],panel[p]/swide);
#endif

        if (VERBOSE)
          { fprintf(stdout,"\r    Sorting seeds for part %d  ",u*NPARTS+i+1);
            fflush(stdout);
          }

        nused = rmsd_sort(sarray,nels,swide,swide-2,NCONTS,panel,NTHREADS,range);

#ifdef DEBUG_SORT
        print_seeds(sarray,swide,range,panel,DB1,DB2,u);
#endif
      }

      if (VERBOSE)
        { fprintf(stdout,"\r    Searching seeds for part %d",u*NPARTS+i+1);
          fflush(stdout);
        }

      for (p = 0; p < nused; p++)
        tarm[p].comp = u;

#if defined(DEBUG_SORT) || defined(DEBUG_SEARCH) || defined(DEBUG_HIT) || defined(DEBUG_ALIGN)
      for (p = 0; p < nused; p++)
        search_seeds(tarm+p);
#else
      for (p = 1; p < nused; p++)
        pthread_create(threads+p,NULL,search_seeds,tarm+p);
      search_seeds(tarm);
      for (p = 1; p < nused; p++)
        pthread_join(threads[p],NULL);
#endif
    }

  free(panel);
  free(sarray);

  for (p = 0; p < NTHREADS; p++)
    { fclose(tarm[p].tfile);
      rewind(tarm[p].ofile);
      fwrite(&(tarm[p].nlive),sizeof(int64),1,tarm[p].ofile);
      fclose(tarm[p].ofile);
    }

  for (p = 1; p < NTHREADS; p++)
    { fclose(tarm[p].DB2.bases);
      fclose(tarm[p].DB1.bases);
    }

  if (VERBOSE)
    { int64 nhit, nlas, nliv, ncov;

      fprintf(stdout,"\r    Done                        \n");

      nhit = nlas = nliv = ncov = 0;
      for (p = 0; p < NTHREADS; p++)
        { nhit += tarm[p].nhits;
          nlas += tarm[p].nlass;
          nliv += tarm[p].nlive;
          ncov += tarm[p].nlcov;
        }
      if (nliv == 0)
        fprintf(stdout,
           "\n  Total hits over %d = %lld, %lld la's, 0 non-redundant la's of ave len 0\n",
                       CHAIN_MIN,nhit,nlas);
      else
        fprintf(stdout,
           "\n  Total hits over %d = %lld, %lld la's, %lld non-redundant la's of ave len %lld\n",
                       CHAIN_MIN,nhit,nlas,nliv,ncov/nliv);
    }
}


int main(int argc, char *argv[])
{ Kmer_Stream *T1, *T2;
  Post_List   *P1, *P2;
  DAZZ_DB _DB1, *DB1 = &_DB1;
  DAZZ_DB _DB2, *DB2 = &_DB2;
  char    *OUTP;

  //  Process options

  { int    i, j, k;
    int    flags[128];
    char  *eptr;

    ARG_INIT("FastGA");

    FREQ = -1;
    OUTP = NULL;
    CHAIN_BREAK = 500;
    CHAIN_MIN   = 100;
    ALIGN_MIN   = 100;
    ALIGN_RATE  = .7;
    SORT_PATH   = "/tmp";

    j = 1;
    for (i = 1; i < argc; i++)
      if (argv[i][0] == '-')
        switch (argv[i][1])
        { default:
            ARG_FLAGS("v")
            break;
          case 'a':
            ARG_NON_NEGATIVE(ALIGN_MIN,"minimum alignment length");
            break;
          case 'c':
            ARG_NON_NEGATIVE(CHAIN_MIN,"minimum seed cover");
            break;
          case 'e':
            ARG_REAL(ALIGN_RATE);
            if (ALIGN_RATE < .6 || ALIGN_RATE >= 1.)
              { fprintf(stderr,"%s: '-e' minimum alignment similarity must be in [0.6,1.0)\n",
                               Prog_Name);
                exit (1);
              }
            break;
          case 'f':
            ARG_NON_NEGATIVE(FREQ,"maximum seed frequency");
            break;
          case 'o':
            OUTP = argv[i]+2;
            break;
          case 's':
            ARG_NON_NEGATIVE(CHAIN_BREAK,"seed chain break threshold");
            break;
          case 'P':
            SORT_PATH = argv[i]+2;
            break;
        }
      else
        argv[j++] = argv[i];
    argc = j;

    VERBOSE = flags['v'];

    if (argc != 3 || FREQ < 0)
      { fprintf(stderr,"\nUsage: %s %s\n",Prog_Name,Usage[0]);
        fprintf(stderr,"       %*s %s\n",(int) strlen(Prog_Name),"",Usage[1]);
        fprintf(stderr,"       %*s %s\n",(int) strlen(Prog_Name),"",Usage[2]);
        fprintf(stderr,"\n");
        fprintf(stderr,"      -v: Verbose mode, output statistics as proceed.\n");
        fprintf(stderr,"      -P: Directory to use for temporary files.\n");
        fprintf(stderr,"      -o: Use as root name for output .las file.\n");
        fprintf(stderr,"\n");
        fprintf(stderr,"      -f: adaptive seed count cutoff (mandatory)\n");
        fprintf(stderr,"\n");
        fprintf(stderr,"      -c: minimum seed chain coverage in both genomes\n");
        fprintf(stderr,"      -s: threshold for starting a new seed chain\n");
        fprintf(stderr,"      -a: minimum alignment length\n");
        fprintf(stderr,"      -e: minimum alignment similarity\n");
        fprintf(stderr,"\n");
        exit (1);
      }
  }

  //  Get full path strong for sorting subdirectory (in variable SORT_PATH)

  { char  *cpath, *spath;
    DIR   *dirp;

    if (SORT_PATH[0] != '/')
      { cpath = getcwd(NULL,0);
        if (SORT_PATH[0] == '.')
          { if (SORT_PATH[1] == '/')
              spath = Catenate(cpath,SORT_PATH+1,"","");
            else if (SORT_PATH[1] == '\0')
              spath = cpath;
            else
              { fprintf(stderr,"\n%s: -P option: . not followed by /\n",Prog_Name);
                exit (1);
              }
          }
        else
          spath = Catenate(cpath,"/",SORT_PATH,"");
        SORT_PATH = Strdup(spath,"Allocating path");
        free(cpath);
      }
    else
      SORT_PATH = Strdup(SORT_PATH,"Allocating path");

    if ((dirp = opendir(SORT_PATH)) == NULL)
      { fprintf(stderr,"\n%s: -P option: cannot open directory %s\n",Prog_Name,SORT_PATH);
        exit (1);
      }
    closedir(dirp);
  }

  T1 = Open_Kmer_Stream(argv[1]);
  T2 = Open_Kmer_Stream(argv[2]);
  if (T1 == NULL)
    { fprintf(stderr,"%s: Cannot find genome index for %s\n",Prog_Name,argv[1]);
      exit (1);
    }
  if (T2 == NULL)
    { fprintf(stderr,"%s: Cannot find genome index for %s\n",Prog_Name,argv[2]);
      exit (1);
    }
  
  P1 = Open_Post_List(argv[1]);
  P2 = Open_Post_List(argv[2]);
  if (P1 == NULL)
    { fprintf(stderr,"%s: Cannot find genome index for %s\n",Prog_Name,argv[1]);
      exit (1);
    }
  if (P2 == NULL)
    { fprintf(stderr,"%s: Cannot find genome index for %s\n",Prog_Name,argv[2]);
      exit (1);
    }

  Perm1  = P1->perm;
  Perm2  = P2->perm;

  if (Open_DB(argv[1],DB1) < 0)
    exit (1);
  Trim_DB(DB1);

  if (Open_DB(argv[2],DB2) < 0)
    exit (1);
  Trim_DB(DB2);

  KMER      = T1->kmer;
  NTHREADS  = P1->nsqrt;
  if (P2->nsqrt != NTHREADS)
    { fprintf(stderr,"%s: Genome indices %s & %s built with different # of threads\n",
                      Prog_Name,argv[1],argv[2]);
      exit (1);
    }

  { char *r1, *r2;

    if (OUTP == NULL)
      { r1 = Root(argv[1],".dam");
        r2 = Root(argv[2],".dam");
        ALGN_NAME = Strdup(Catenate(r1,".",r2,""),"Allocating alignment name");
        free(r2);
        free(r1);
      }
    else
      ALGN_NAME = Strdup(OUTP,"Allocating alignment name");
    ALGN_UNIQ = Strdup(Numbered_Suffix("_uniq.",getpid(),""),"Allocating temp name");
    PAIR_NAME = Strdup(Numbered_Suffix("_pair.",getpid(),""),"Allocating temp name");
    ALGN_PAIR = Strdup(Numbered_Suffix("_algn.",getpid(),""),"Allocating temp name");
    if (ALGN_NAME == NULL || ALGN_UNIQ == NULL || PAIR_NAME == NULL || ALGN_PAIR == NULL)
      exit (1);
  }

  if (P1->freq < FREQ)
    { fprintf(stderr,"%s: Genome index for %s cutoff %d < requested cutoff\n",
                     Prog_Name,argv[1],P1->freq);
      exit (1);
    }
  if (P2->freq < FREQ)
    { fprintf(stderr,"%s: Genome index for %s cutoff %d < requested cutoff\n",
                     Prog_Name,argv[2],P2->freq);
      exit (1);
    }
  if (T1->kmer != T2->kmer)
    { fprintf(stderr,"%s: Indices not made with the same k-mer size (%d vs %d)\n",
                     Prog_Name,T1->kmer,T2->kmer);
      exit (1);
    }

  IBYTE = P1->pbyte;
  ICONT = P1->cbyte;
  IPOST = IBYTE-ICONT;
  ISIGN = IBYTE-1;

  JBYTE = P2->pbyte;
  JCONT = P2->cbyte;
  JPOST = JBYTE-JCONT;
  JSIGN = JBYTE-1;

  KBYTE = T2->pbyte;
  CBYTE = T2->hbyte;
  LBYTE = CBYTE+1;
  if (IPOST > JPOST)
    DBYTE = IPOST;
  else
    DBYTE = JPOST;
  ESHIFT = 8*IPOST;

  if (TSPACE < TRACE_XOVR)
    { ABYTE  = 1;
      TBYTES = sizeof(uint8);
    }
  else
    { ABYTE  = 0;
      TBYTES = sizeof(uint16);
    }

  if (VERBOSE)
    { fprintf(stdout,"\n  Using %d threads\n\n",NTHREADS);
      fflush(stdout);
    }

  { int64 npost, cum, t;   //  Compute DB split into NTHREADS parts
    int   p, r, x;

    NCONTS = DB1->treads;

    IDBsplit = Malloc((NTHREADS+1)*sizeof(int),"Allocating DB1 partitions");
    Select   = Malloc(NCONTS*sizeof(int),"Allocating DB1 partition");
    if (IDBsplit == NULL || Select == NULL)
      exit (1);

    npost = DB1->totlen;
    IDBsplit[0] = 0;
    Select[0] = 0;
    p = 0;
    r = NTHREADS;
    t = npost/NTHREADS;
    cum = DB1->reads[Perm1[0]].rlen;
    for (x = 1; x < NCONTS; x++)
      { if (cum >= t && x >= r)
          { p += 1;
            IDBsplit[p] = x;
            t = (npost*(p+1))/NTHREADS;
            r += NTHREADS;
          }
        Select[x] = p;
        cum += DB1->reads[Perm1[x]].rlen;
      }
    NPARTS = p+1;
    IDBsplit[NPARTS] = NCONTS;

#ifdef DEBUG_SPLIT
    for (x = 0; x < NPARTS; x++)
      { printf(" %2d: %4d - %4d\n",x,IDBsplit[x],IDBsplit[x+1]);
        for (r = IDBsplit[x]; r < IDBsplit[x+1]; r++)
          if (Select[r] != x)
            printf("  Not OK: %d->%d\n",r,Select[x]);
      }
#endif
  }

  { int    i, j, k, x;   // Setup temporary pair file IO buffers
    uint8 *buffer;
    int64 *bucks;
    char  *names;
    int    namelen = strlen(PAIR_NAME)+strlen(SORT_PATH)+16;

    N_Units = Malloc(NPARTS*NTHREADS*sizeof(IOBuffer),"IO buffers");
    C_Units = Malloc(NPARTS*NTHREADS*sizeof(IOBuffer),"IO buffers");
    buffer  = Malloc(2*NPARTS*NTHREADS*1000000,"IO buffers");
    bucks   = Malloc(2*NTHREADS*NCONTS*sizeof(int64),"IO buffers");
    names   = Malloc(2*NPARTS*NTHREADS*namelen,"IO names");
    if (N_Units == NULL || C_Units == NULL || buffer == NULL || bucks == NULL || names == NULL)
      exit (1);

    k = 0;
    for (i = 0; i < NTHREADS; i++)
      for (j = 0; j < NPARTS; j++)
        { N_Units[k].bufr = buffer + (2*k) * 1000000; 
          C_Units[k].bufr = buffer + (2*k+1) * 1000000; 
          N_Units[k].name = names + (2*k) * namelen;
          C_Units[k].name = names + (2*k+1) * namelen;
          sprintf(N_Units[k].name,"%s/%s.%d.N",SORT_PATH,PAIR_NAME,k);
          sprintf(C_Units[k].name,"%s/%s.%d.C",SORT_PATH,PAIR_NAME,k);
          N_Units[k].buck = bucks + (2*i) * NCONTS; 
          C_Units[k].buck = bucks + (2*i+1) * NCONTS; 
          k += 1;
        }

#ifdef DEBUG_SPLIT
    for (i = 0; i < NTHREADS; i++)
      { for (j = 0; j < NPARTS; j++)
          printf("%s ",N_Units[i*NPARTS+j].name);
        printf("\n");
        for (j = 0; j < NPARTS; j++)
          printf("%ld ",N_Units[i*NPARTS+j].buck-bucks);
        printf("\n");
        for (j = 0; j < NPARTS; j++)
          printf("%ld ",N_Units[i*NPARTS+j].bufr-buffer);
        printf("\n");
      }
#endif

    adaptamer_merge(argv[1],argv[2],T1,T2,P1,P2);

    //  Effectively transpose N_unit & C_unit matrices

    k = 0;
    for (j = 0; j < NPARTS; j++)
      for (i = 0; i < NTHREADS; i++)
        { N_Units[k].bufr = 
          C_Units[k].bufr = buffer + i * (2*NPARTS*1000000); 
          x = i*NPARTS+j;
          N_Units[k].name = names + (2*x) * namelen;
          C_Units[k].name = names + (2*x+1) * namelen;
          N_Units[k].buck = bucks + (2*i) * NCONTS; 
          C_Units[k].buck = bucks + (2*i+1) * NCONTS; 
          k += 1;
        }

#ifdef DEBUG_SPLIT
    for (j = 0; j < NPARTS; j++)
      { for (i = 0; i < NTHREADS; i++)
          printf("%s ",N_Units[j*NTHREADS+i].name);
        printf("\n");
        for (i = 0; i < NTHREADS; i++)
          printf("%ld ",N_Units[j*NTHREADS+i].buck-bucks);
        printf("\n");
        for (i = 0; i < NTHREADS; i++)
          printf("%ld ",N_Units[j*NTHREADS+i].bufr-buffer);
        printf("\n");
      }
#endif
  
    pair_sort_search(DB1,DB2);

    free(N_Units->name);
    free(N_Units->buck);
    free(N_Units->bufr);
    free(C_Units);
    free(N_Units);
  }

  { char *command;

    if (VERBOSE)
      fprintf(stdout,"\nSorting and merging local alignments\n");

    command = Malloc(2*strlen(ALGN_UNIQ)+strlen(SORT_PATH)+500,"Command string");
    if (command == NULL)
      exit (1);

    sprintf(command,"LAsort -a %s/%s.*.las",SORT_PATH,ALGN_UNIQ); 
    if (system(command) != 0)
      { fprintf(stderr,"%s: Alignment sorts with LAsort failed. ?\n",Prog_Name);
        sprintf(command,"rm -f %s/%s.*.las %s/%s.*.S.las",SORT_PATH,ALGN_UNIQ,SORT_PATH,ALGN_UNIQ); 
        system(command);
        exit (1);
      }

    sprintf(command,"LAmerge -a %s.las %s/%s.*.S.las",ALGN_NAME,SORT_PATH,ALGN_UNIQ); 
    if (system(command) != 0)
      { fprintf(stderr,"%s: Alignment merge with LAmerge failed. ?\n",Prog_Name);
        sprintf(command,"rm -f %s/%s.*.las %s/%s.*.S.las",SORT_PATH,ALGN_UNIQ,SORT_PATH,ALGN_UNIQ); 
        system(command);
        exit (1);
      }

    sprintf(command,"rm -f %s/%s.*.las %s/%s.*.las %s/%s.*.S.las",
                    SORT_PATH,ALGN_PAIR,SORT_PATH,ALGN_UNIQ,SORT_PATH,ALGN_UNIQ); 
    if (system(command) != 0)
      { fprintf(stderr,"%s: Could not remove intermediate alignment files. ?\n",Prog_Name);
        exit (1);
      }

    free(command);
  }

  free(Select);
  free(IDBsplit);

  free(ALGN_UNIQ);
  free(ALGN_NAME);
  free(PAIR_NAME);

  Free_Post_List(P1);
  Free_Post_List(P2);

  Catenate(NULL,NULL,NULL,NULL);
  Numbered_Suffix(NULL,0,NULL);
  free(Prog_Name);

  exit (0);
}
