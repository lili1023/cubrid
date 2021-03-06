/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * unittest_lf.c : unit tests for latch free primitives
 */

#include "porting.h"
#include "lock_free.h"
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>

/* wait-free random number array */
#define RAND_BLOCKS	64
#define RAND_BLOCK_SIZE	1000000
#define RAND_SIZE	RAND_BLOCKS * RAND_BLOCK_SIZE
static int random_numbers[RAND_SIZE];

static void
generate_random ()
{
  int i = 0;

  srand (time (NULL));

  for (i = 0; i < RAND_SIZE; i++)
    {
      random_numbers[i] = rand ();
    }
}

/* hash entry type definition */
typedef struct xentry XENTRY;
struct xentry
{
  XENTRY *next;			/* used by hash and freelist */
  XENTRY *stack;		/* used by freelist */
  UINT64 del_tran_id;		/* used by freelist */

  pthread_mutex_t mutex;	/* entry mutex (where applicable) */

  int key;
  unsigned long long int data;
};

/* entry manipulation functions */
static void *
xentry_alloc ()
{
  XENTRY *ptr = (XENTRY *) malloc (sizeof (XENTRY));
  if (ptr != NULL)
    {
      pthread_mutex_init (&ptr->mutex, NULL);
      ptr->data = 0;
    }
  return ptr;
}

static int
xentry_free (void *entry)
{
  pthread_mutex_destroy (&((XENTRY *) entry)->mutex);
  free (entry);
  return NO_ERROR;
}

static int
xentry_init (void *entry)
{
  if (entry != NULL)
    {
      ((XENTRY *) entry)->data = 0;
      return NO_ERROR;
    }
  else
    {
      return ER_FAILED;
    }
}

static int
xentry_uninit (void *entry)
{
  if (entry != NULL)
    {
      ((XENTRY *) entry)->data = -1;
      return NO_ERROR;
    }
  else
    {
      return ER_FAILED;
    }

}

static unsigned int
xentry_hash (void *key, int htsize)
{
  int *ikey = (int *) key;
  return (*ikey) % htsize;
}

static int
xentry_key_compare (void *k1, void *k2)
{
  int *ik1 = (int *) k1, *ik2 = (int *) k2;
  return !(*ik1 == *ik2);
}

static int
xentry_key_copy (void *src, void *dest)
{
  int *isrc = (int *) src, *idest = (int *) dest;

  *idest = *isrc;

  return NO_ERROR;
}

/* hash entry descriptors */
static LF_ENTRY_DESCRIPTOR xentry_desc = {
  /* signature */
  offsetof (XENTRY, stack),
  offsetof (XENTRY, next),
  offsetof (XENTRY, del_tran_id),
  offsetof (XENTRY, key),
  offsetof (XENTRY, mutex),

  /* mutex flags */
  LF_EM_NOT_USING_MUTEX,

  /* functions */
  xentry_alloc,
  xentry_free,
  xentry_init,
  xentry_uninit,
  xentry_key_copy,
  xentry_key_compare,
  xentry_hash
};

/* print function */
static struct timeval start_time;

static void
begin (char *test_name)
{
#define MSG_LEN	  40
  int i;

  printf ("Testing %s", test_name);
  for (i = 0; i < MSG_LEN - strlen (test_name); i++)
    {
      putchar (' ');
    }
  printf ("...");

  gettimeofday (&start_time, NULL);

#undef MSG_LEN
}

static int
fail (const char *message)
{
  printf (" %s: %s\n", "FAILED", message);
  assert (false);
  return ER_FAILED;
}

static int
success ()
{
  struct timeval end_time;
  long long int elapsed_msec = 0;

  gettimeofday (&end_time, NULL);

  elapsed_msec = (end_time.tv_usec - start_time.tv_usec) / 1000;
  elapsed_msec += (end_time.tv_sec - start_time.tv_sec) * 1000;

  printf (" %s [%9.3f sec]\n", "OK", (float) elapsed_msec / 1000.0f);
  return NO_ERROR;
}

/* thread entry functions */
void *
test_freelist_proc (void *param)
{
#define NOPS	  1000000	/* 1M */

  LF_FREELIST *freelist = (LF_FREELIST *) param;
  LF_TRAN_SYSTEM *ts = freelist->tran_system;
  LF_TRAN_ENTRY *te;
  XENTRY *entry;
  int i;

  te = lf_tran_request_entry (ts);
  if (te == NULL)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  for (i = 0; i < NOPS; i++)
    {
      if (lf_tran_start (te, true) != NO_ERROR)
	{
	  pthread_exit (ER_FAILED);
	  return ER_FAILED;
	}

      if (i % 2 == 0)
	{
	  entry = (XENTRY *) lf_freelist_claim (te, freelist);
	  if (entry == NULL)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	}
      else
	{
	  if (lf_freelist_retire (te, freelist, (void *) entry) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	}

      if (lf_tran_end (te) != NO_ERROR)
	{
	  pthread_exit (ER_FAILED);
	  return ER_FAILED;
	}
    }

  if (lf_tran_return_entry (te) != NO_ERROR)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  pthread_exit (NO_ERROR);

#undef NOPS
}

void *
test_hash_proc_1 (void *param)
{
#define NOPS  1000000

  LF_HASH_TABLE *hash = (LF_HASH_TABLE *) param;
  LF_TRAN_SYSTEM *ts = hash->freelist->tran_system;
  LF_TRAN_ENTRY *te;
  XENTRY *entry;
  int i, rand_base, key;

  te = lf_tran_request_entry (ts);
  if (te == NULL)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  if (te->entry_idx >= RAND_BLOCKS || te->entry_idx < 0)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }
  else
    {
      rand_base = te->entry_idx * RAND_BLOCK_SIZE;
    }

  for (i = 0; i < NOPS; i++)
    {
      key = random_numbers[rand_base + i] % 1000;

      if (i % 10 < 5)
	{
	  entry = NULL;
	  if (lf_hash_find_or_insert (te, hash, &key, &entry) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	  lf_tran_end (te);
	}
      else
	{
	  if (lf_hash_delete (te, hash, &key, NULL) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	}
    }

  if (lf_tran_return_entry (te) != NO_ERROR)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  pthread_exit (NO_ERROR);

#undef NOPS
}

void *
test_hash_proc_2 (void *param)
{
#define NOPS  1000000

  LF_HASH_TABLE *hash = (LF_HASH_TABLE *) param;
  LF_TRAN_SYSTEM *ts = hash->freelist->tran_system;
  LF_TRAN_ENTRY *te;
  XENTRY *entry;
  int i, rand_base, key;

  te = lf_tran_request_entry (ts);
  if (te == NULL)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  if (te->entry_idx >= RAND_BLOCKS || te->entry_idx < 0)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }
  else
    {
      rand_base = te->entry_idx * RAND_BLOCK_SIZE;
    }

  for (i = 0; i < NOPS; i++)
    {
      key = random_numbers[rand_base + i] % 1000;

      if (i % 10 < 5)
	{
	  if (lf_hash_find_or_insert (te, hash, &key, &entry) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	  if (entry == NULL)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }

	  pthread_mutex_unlock (&entry->mutex);
	}
      else
	{
	  if (lf_hash_delete (te, hash, &key, NULL) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	}
    }

  if (lf_tran_return_entry (te) != NO_ERROR)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }


  pthread_exit (NO_ERROR);

#undef NOPS
}

static int del_op_count = 0;

void *
test_hash_proc_3 (void *param)
{
#define NOPS  1000000

  LF_HASH_TABLE *hash = (LF_HASH_TABLE *) param;
  LF_TRAN_SYSTEM *ts = hash->freelist->tran_system;
  LF_TRAN_ENTRY *te;
  XENTRY *entry;
  int i, rand_base, key, local_del_op_count = 0;

  te = lf_tran_request_entry (ts);
  if (te == NULL)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  if (te->entry_idx >= RAND_BLOCKS || te->entry_idx < 0)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }
  else
    {
      rand_base = te->entry_idx * RAND_BLOCK_SIZE;
    }

  for (i = 0; i < NOPS; i++)
    {
      key = random_numbers[rand_base + i] % 1000;

      if (lf_hash_find_or_insert (te, hash, &key, &entry) != NO_ERROR)
	{
	  pthread_exit (ER_FAILED);
	  return ER_FAILED;
	}
      if (entry == NULL)
	{
	  pthread_exit (ER_FAILED);
	  return ER_FAILED;
	}
      if (entry->key != key)
	{
	  pthread_exit (ER_FAILED);
	  return ER_FAILED;
	}

      entry->data++;

      if (entry->data >= 10)
	{
	  int success = 0;

	  local_del_op_count += entry->data;
	  if (lf_hash_delete (te, hash, &key, &success) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	  else if (!success)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	}
      else
	{
	  pthread_mutex_unlock (&entry->mutex);
	}
    }

  if (lf_tran_return_entry (te) != NO_ERROR)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  ATOMIC_INC_32 (&del_op_count, local_del_op_count);
  pthread_exit (NO_ERROR);

#undef NOPS
}

void *
test_clear_proc_1 (void *param)
{
#define NOPS  1000000

  LF_HASH_TABLE *hash = (LF_HASH_TABLE *) param;
  LF_TRAN_SYSTEM *ts = hash->freelist->tran_system;
  LF_TRAN_ENTRY *te;
  XENTRY *entry;
  int i, rand_base, key;

  te = lf_tran_request_entry (ts);
  if (te == NULL)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  if (te->entry_idx >= RAND_BLOCKS || te->entry_idx < 0)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }
  else
    {
      rand_base = te->entry_idx * RAND_BLOCK_SIZE;
    }

  for (i = 0; i < NOPS; i++)
    {
      key = random_numbers[rand_base + i] % 1000;
      key = i % 100;

      if (i % 1000 != 999)
	{
	  if (i % 10 < 8)
	    {
	      entry = NULL;
	      if (lf_hash_find_or_insert (te, hash, &key, &entry) != NO_ERROR)
		{
		  pthread_exit (ER_FAILED);
		  return ER_FAILED;
		}
	      lf_tran_end (te);
	    }
	  else if (i % 1000 < 999)
	    {
	      if (lf_hash_delete (te, hash, &key, NULL) != NO_ERROR)
		{
		  pthread_exit (ER_FAILED);
		  return ER_FAILED;
		}
	    }
	}
      else
	{
	  if (lf_hash_clear (te, hash) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	}
    }

  if (lf_tran_return_entry (te) != NO_ERROR)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  pthread_exit (NO_ERROR);

#undef NOPS
}

void *
test_clear_proc_2 (void *param)
{
#define NOPS  1000000

  LF_HASH_TABLE *hash = (LF_HASH_TABLE *) param;
  LF_TRAN_SYSTEM *ts = hash->freelist->tran_system;
  LF_TRAN_ENTRY *te;
  XENTRY *entry;
  int i, rand_base, key;

  te = lf_tran_request_entry (ts);
  if (te == NULL)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  if (te->entry_idx >= RAND_BLOCKS || te->entry_idx < 0)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }
  else
    {
      rand_base = te->entry_idx * RAND_BLOCK_SIZE;
    }

  for (i = 0; i < NOPS; i++)
    {
      key = random_numbers[rand_base + i] % 1000;

      if (i % 1000 < 999)
	{
	  if (i % 10 < 5)
	    {
	      if (lf_hash_find_or_insert (te, hash, &key, &entry) != NO_ERROR)
		{
		  pthread_exit (ER_FAILED);
		  return ER_FAILED;
		}
	      if (entry == NULL)
		{
		  pthread_exit (ER_FAILED);
		  return ER_FAILED;
		}

	      pthread_mutex_unlock (&entry->mutex);
	    }
	  else
	    {
	      if (lf_hash_delete (te, hash, &key, NULL) != NO_ERROR)
		{
		  pthread_exit (ER_FAILED);
		  return ER_FAILED;
		}
	    }
	}
      else
	{
	  if (lf_hash_clear (te, hash) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	}
    }

  if (lf_tran_return_entry (te) != NO_ERROR)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }


  pthread_exit (NO_ERROR);

#undef NOPS
}

void *
test_clear_proc_3 (void *param)
{
#define NOPS  1000000

  LF_HASH_TABLE *hash = (LF_HASH_TABLE *) param;
  LF_TRAN_SYSTEM *ts = hash->freelist->tran_system;
  LF_TRAN_ENTRY *te;
  XENTRY *entry;
  int i, rand_base, key;

  te = lf_tran_request_entry (ts);
  if (te == NULL)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  if (te->entry_idx >= RAND_BLOCKS || te->entry_idx < 0)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }
  else
    {
      rand_base = te->entry_idx * RAND_BLOCK_SIZE;
    }

  for (i = 0; i < NOPS; i++)
    {
      key = random_numbers[rand_base + i] % 1000;

      if (i % 1000 == 999)
	{
	  if (lf_hash_clear (te, hash) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }

	  continue;
	}

      if (lf_hash_find_or_insert (te, hash, &key, &entry) != NO_ERROR)
	{
	  pthread_exit (ER_FAILED);
	  return ER_FAILED;
	}
      if (entry == NULL)
	{
	  pthread_exit (ER_FAILED);
	  return ER_FAILED;
	}

      entry->data++;

      if (entry->data >= 10)
	{
	  int success = 0;

	  if (lf_hash_delete (te, hash, &key, &success) != NO_ERROR)
	    {
	      pthread_exit (ER_FAILED);
	      return ER_FAILED;
	    }
	  else if (!success)
	    {
	      /* cleared in the meantime */
	      pthread_mutex_unlock (&entry->mutex);
	    }
	}
      else
	{
	  pthread_mutex_unlock (&entry->mutex);
	}

      lf_tran_end (te);
    }

  if (lf_tran_return_entry (te) != NO_ERROR)
    {
      pthread_exit (ER_FAILED);
      return ER_FAILED;
    }

  del_op_count = -1;

  pthread_exit (NO_ERROR);

#undef NOPS
}

/* test functions */
static int
test_freelist (LF_ENTRY_DESCRIPTOR * edesc, int nthreads)
{
#define MAX_THREADS 64

  static LF_FREELIST freelist;
  static LF_TRAN_SYSTEM ts;
  pthread_t threads[MAX_THREADS];
  char msg[256];
  int i;

  sprintf (msg, "freelist (%d threads)", nthreads);
  begin (msg);

  /* initialization */
  if (nthreads > MAX_THREADS)
    {
      return fail ("too many threads");
    }

  if (lf_tran_system_init (&ts, nthreads) != NO_ERROR)
    {
      return fail ("transaction system init");
    }

  if (lf_freelist_init (&freelist, 100, 100, edesc, &ts) != NO_ERROR)
    {
      return fail ("freelist init");
    }

  /* multithreaded test */
  for (i = 0; i < nthreads; i++)
    {
      if (pthread_create (&threads[i], NULL, test_freelist_proc, (void *) &freelist) != NO_ERROR)
	{
	  return fail ("thread create");
	}
    }

  for (i = 0; i < nthreads; i++)
    {
      void *retval;

      pthread_join (threads[i], &retval);
      if (retval != NO_ERROR)
	{
	  return fail ("thread proc error");
	}
    }

  /* results */
  {
    volatile XENTRY *e, *a, *r;
    volatile int active, retired, _a, _r, _t;

    a = VOLATILE_ACCESS (freelist.available, void *);

    _a = VOLATILE_ACCESS (freelist.available_cnt, int);
    _r = VOLATILE_ACCESS (freelist.retired_cnt, int);
    _t = VOLATILE_ACCESS (freelist.alloc_cnt, int);

    active = 0;
    retired = 0;
    for (e = (XENTRY *) a; e != NULL; e = e->stack)
      {
	active++;
      }
    for (i = 0; i < ts.entry_count; i++)
      {
	for (e = (XENTRY *) ts.entries[i].retired_list; e != NULL; e = e->stack)
	  {
	    retired++;
	  }
      }

    if ((_t - active - retired) != 0)
      {
	sprintf (msg, "leak problem (lost %d entries)", _t - active + retired);
	return fail (msg);
      }

    if ((active != _a) || (retired != _r))
      {
	sprintf (msg, "counting problem (%d != %d) or (%d != %d)", active, _a, retired, _r);
	return fail (msg);
      }
  }

  /* uninit */
  lf_freelist_destroy (&freelist);
  lf_tran_system_destroy (&ts);

  return success ();

#undef MAX_THREADS
}

static int
test_hash_table (LF_ENTRY_DESCRIPTOR * edesc, int nthreads, void *(*proc) (void *))
{
#define MAX_THREADS	  1024
#define HASH_SIZE	  113

  static LF_FREELIST freelist;
  static LF_TRAN_SYSTEM ts;
  static LF_HASH_TABLE hash;
  pthread_t threads[MAX_THREADS];
  char msg[256];
  int i;

  del_op_count = 0;

  sprintf (msg, "hash (lf=%s, ld=%s, ud=%s, %d threads)", (edesc->mutex_flags & LF_EM_FLAG_LOCK_ON_FIND ? "y" : "n"),
	   (edesc->mutex_flags & LF_EM_FLAG_LOCK_ON_DELETE ? "y" : "n"),
	   (edesc->mutex_flags & LF_EM_FLAG_UNLOCK_AFTER_DELETE ? "y" : "n"), nthreads);
  begin (msg);

  /* initialization */
  if (nthreads > MAX_THREADS)
    {
      return fail ("too many threads");
    }

  if (lf_tran_system_init (&ts, nthreads) != NO_ERROR)
    {
      return fail ("transaction system init");
    }

  if (lf_freelist_init (&freelist, 100, 100, edesc, &ts) != NO_ERROR)
    {
      return fail ("freelist init");
    }

  if (lf_hash_init (&hash, &freelist, HASH_SIZE, edesc) != NO_ERROR)
    {
      return fail ("hashtable init");
    }

  /* multithreaded test */
  for (i = 0; i < nthreads; i++)
    {
      if (pthread_create (&threads[i], NULL, proc, (void *) &hash) != NO_ERROR)
	{
	  return fail ("thread create");
	}
    }

  for (i = 0; i < nthreads; i++)
    {
      void *retval;

      pthread_join (threads[i], &retval);
      if (retval != NO_ERROR)
	{
	  return fail ("thread proc error");
	}
    }

  /* count operations */
  if (edesc->mutex_flags == (LF_EM_FLAG_LOCK_ON_FIND | LF_EM_FLAG_UNLOCK_AFTER_DELETE))
    {
      XENTRY *e;
      int nondel_op_count = 0;

      for (i = 0; i < HASH_SIZE; i++)
	{
	  for (e = hash.buckets[i]; e != NULL; e = e->next)
	    {
	      nondel_op_count += e->data;
	    }
	}

      if (del_op_count != -1)
	{
	  /* we're counting delete ops */
	  if (nondel_op_count + del_op_count != nthreads * 1000000)
	    {
	      sprintf (msg, "op count fail (%d + %d != %d)", nondel_op_count, del_op_count, nthreads * 1000000);
	      return fail (msg);
	    }
	}
    }

  /* count entries */
  {
    XENTRY *e;
    int ecount = 0, acount = 0, rcount = 0;

    for (i = 0; i < HASH_SIZE; i++)
      {
	for (e = hash.buckets[i]; e != NULL; e = e->next)
	  {
	    ecount++;
	  }
      }

    for (e = freelist.available; e != NULL; e = e->stack)
      {
	acount++;
      }
    for (i = 0; i < ts.entry_count; i++)
      {
	for (e = ts.entries[i].retired_list; e != NULL; e = e->stack)
	  {
	    rcount++;
	  }
	if (ts.entries[i].temp_entry != NULL)
	  {
	    ecount++;
	  }
      }

    if (freelist.available_cnt != acount)
      {
	sprintf (msg, "counting fail (available %d != %d)", freelist.available_cnt, acount);
	return fail (msg);
      }
    if (freelist.retired_cnt != rcount)
      {
	sprintf (msg, "counting fail (retired %d != %d)", freelist.retired_cnt, rcount);
	return fail (msg);
      }

    if (ecount + freelist.available_cnt + freelist.retired_cnt != freelist.alloc_cnt)
      {
	sprintf (msg, "leak check fail (%d + %d + %d != %d)", ecount, freelist.available_cnt, freelist.retired_cnt,
		 freelist.alloc_cnt);
	return fail (msg);
      }
  }

  /* uninit */
  lf_hash_destroy (&hash);
  lf_freelist_destroy (&freelist);
  lf_tran_system_destroy (&ts);

  return success ();
#undef HASH_SIZE
#undef MAX_THREADS
}

static int
test_hash_iterator ()
{
#define HASH_SIZE 200
#define HASH_POPULATION HASH_SIZE * 5
#define NUM_THREADS 16

  static LF_FREELIST freelist;
  static LF_TRAN_SYSTEM ts;
  static LF_HASH_TABLE hash;
  static LF_TRAN_ENTRY *te;
  pthread_t threads[NUM_THREADS];
  int i;

  del_op_count = 0;

  begin ("hash table iterator");

  /* initialization */
  if (lf_tran_system_init (&ts, NUM_THREADS) != NO_ERROR)
    {
      return fail ("transaction system init");
    }

  te = lf_tran_request_entry (&ts);
  if (te == NULL)
    {
      return fail ("failed to fetch tran entry");
    }

  if (lf_freelist_init (&freelist, 100, 100, &xentry_desc, &ts) != NO_ERROR)
    {
      return fail ("freelist init");
    }

  if (lf_hash_init (&hash, &freelist, HASH_SIZE, &xentry_desc) != NO_ERROR)
    {
      return fail ("hashtable init");
    }

  /* static (single threaded) test */
  for (i = 0; i < HASH_POPULATION; i++)
    {
      XENTRY *entry;

      if (lf_hash_find_or_insert (te, &hash, &i, &entry) != NO_ERROR)
	{
	  return fail ("insert error");
	}
      else if (entry == NULL)
	{
	  return fail ("null insert error");
	}
      else
	{
	  entry->data = i;
	}
    }

  {
    LF_HASH_TABLE_ITERATOR it;
    XENTRY *curr = NULL;
    char msg[256];
    int sum = 0;

    lf_hash_create_iterator (&it, te, &hash);
    for (curr = lf_hash_iterate (&it); curr != NULL; curr = lf_hash_iterate (&it))
      {
	sum += ((XENTRY *) curr)->data;
      }

    if (sum != ((HASH_POPULATION - 1) * HASH_POPULATION) / 2)
      {
	sprintf (msg, "counting error (%d != %d)", sum, (HASH_POPULATION - 1) * HASH_POPULATION / 2);
	return fail (msg);
      }
  }

  /* reset */
  if (lf_hash_clear (te, &hash) != NO_ERROR)
    {
      return fail ("clear error");
    }

  /* multi-threaded test */
  /* TODO TODO TODO */

  /* uninit */
  lf_tran_return_entry (te);
  lf_hash_destroy (&hash);
  lf_freelist_destroy (&freelist);
  lf_tran_system_destroy (&ts);
  return success ();
#undef HASH_SIZE
#undef HASH_POPULATION
#undef NUM_THREADS
}

/* program entry */
int
main (int argc, char **argv)
{
  int i;

  /* generate random number array for non-blocking access */
  generate_random ();

  /* circular queue */
  /* temporarily disabled */
  /* for (i = 1; i <= 64; i *= 2) { if (test_circular_queue (i) != NO_ERROR) { goto fail; } } */

  /* freelist */
  for (i = 1; i <= 64; i *= 2)
    {
      if (test_freelist (&xentry_desc, i) != NO_ERROR)
	{
	  goto fail;
	}
    }

  /* test hash table iterator */
  if (test_hash_iterator () != NO_ERROR)
    {
      goto fail;
    }

  /* hash table - no entry mutex */
  for (i = 1; i <= 64; i *= 2)
    {
      if (test_hash_table (&xentry_desc, i, test_hash_proc_1) != NO_ERROR)
	{
	  goto fail;
	}
      if (test_hash_table (&xentry_desc, i, test_clear_proc_1) != NO_ERROR)
	{
	  goto fail;
	}
    }

  /* hash table - entry mutex, no lock between find and delete */
  xentry_desc.mutex_flags = LF_EM_FLAG_LOCK_ON_FIND | LF_EM_FLAG_LOCK_ON_DELETE | LF_EM_FLAG_UNLOCK_AFTER_DELETE;
  for (i = 1; i <= 64; i *= 2)
    {
      if (test_hash_table (&xentry_desc, i, test_hash_proc_2) != NO_ERROR)
	{
	  goto fail;
	}
      if (test_hash_table (&xentry_desc, i, test_clear_proc_2) != NO_ERROR)
	{
	  goto fail;
	}

    }

  /* hash table - entry mutex, hold lock between find and delete */
  xentry_desc.mutex_flags = LF_EM_FLAG_LOCK_ON_FIND | LF_EM_FLAG_UNLOCK_AFTER_DELETE;
  for (i = 1; i <= 64; i *= 2)
    {
      if (test_hash_table (&xentry_desc, i, test_hash_proc_3) != NO_ERROR)
	{
	  goto fail;
	}
      if (test_hash_table (&xentry_desc, i, test_clear_proc_3) != NO_ERROR)
	{
	  goto fail;
	}

    }

  /* all ok */
  return 0;

fail:
  printf ("Unit tests failed!\n");
  return ER_FAILED;
}
