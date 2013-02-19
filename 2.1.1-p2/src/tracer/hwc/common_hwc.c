/*****************************************************************************\
 *                        ANALYSIS PERFORMANCE TOOLS                         *
 *                                   Extrae                                  *
 *              Instrumentation package for parallel applications            *
 *****************************************************************************
 *     ___     This library is free software; you can redistribute it and/or *
 *    /  __         modify it under the terms of the GNU LGPL as published   *
 *   /  /  _____    by the Free Software Foundation; either version 2.1      *
 *  /  /  /     \   of the License, or (at your option) any later version.   *
 * (  (  ( B S C )                                                           *
 *  \  \  \_____/   This library is distributed in hope that it will be      *
 *   \  \__         useful but WITHOUT ANY WARRANTY; without even the        *
 *    \___          implied warranty of MERCHANTABILITY or FITNESS FOR A     *
 *                  PARTICULAR PURPOSE. See the GNU LGPL for more details.   *
 *                                                                           *
 * You should have received a copy of the GNU Lesser General Public License  *
 * along with this library; if not, write to the Free Software Foundation,   *
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA          *
 * The GNU LEsser General Public License is contained in the file COPYING.   *
 *                                 ---------                                 *
 *   Barcelona Supercomputing Center - Centro Nacional de Supercomputacion   *
\*****************************************************************************/

/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- *\
 | @file: $HeadURL$
 | @last_commit: $Date$
 | @version:     $Revision$
\* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- */
#include "common.h"

static char UNUSED rcsid[] = "$Id$";

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_STRING_H
# include <string.h>
#endif
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif
#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif

#include "utils.h"
#include "events.h"
#include "clock.h"
#include "threadid.h"
#include "record.h"
#include "trace_macros.h"
#include "wrapper.h"
#include "stdio.h"
#include "xml-parse.h"
#include "common_hwc.h"

/*------------------------------------------------ Global Variables ---------*/
int HWCEnabled = FALSE;           /* Have the HWC been started? */

#if defined(SAMPLING_SUPPORT)
int SamplingSupport = FALSE;
int EnabledSampling = TRUE;
#endif

#if !defined(SAMPLING_SUPPORT)
int Reset_After_Read = TRUE;
#else
int Reset_After_Read = FALSE;
#endif

/* XXX: This variable should be defined at the Extrae core level */
int Trace_HWC_Enabled = TRUE;     /* Global variable that allows the gathering of HWC information */

int *HWC_Thread_Initialized;

/* XXX: These buffers should probably be external to this module, 
   and HWC_Accum should receive the buffer as an I/O parameter. */
long long **Accumulated_HWC;
int *Accumulated_HWC_Valid; /* Marks whether Accumulated_HWC has valid values */


/*------------------------------------------------ Static Variables ---------*/

#if defined(PAPI_COUNTERS) && !defined(PAPIv3)
# error "-DNEW_HWC_SYSTEM requires PAPI v3 support"
#endif

struct HWC_Set_t *HWC_sets = NULL;
unsigned long long HWC_current_changeat = 0;
unsigned long long * HWC_current_timebegin;
enum ChangeType_t HWC_current_changetype = CHANGE_NEVER;
int HWC_num_sets = 0;
int * HWC_current_set;

/**
 * Checks whether the module has been started and the HWC are counting
 * \return 1 if HWC's are enabled, 0 otherwise.
 */
int HWC_IsEnabled()
{
	return HWCEnabled;
}

/**
 * Returns the active counters set (0 .. n-1). 
 * \return The active counters set (0 .. n-1).
 */
int HWC_Get_Current_Set (int threadid)
{
	return HWC_current_set[threadid];
}

/*
 * Returns the total number of sets.
 * \return The total number of sets. 
 */
int HWC_Get_Num_Sets ()
{
	return HWC_num_sets;
}

/**
 * Returns the Paraver IDs for the counters of the given set.
 * \param set_id The set identifier.
 * \param io_HWCParaverIds Buffer where the Paraver IDs will be stored. 
 * \return The number of counters in the given set. 
 */

int HWC_Get_Set_Counters_Ids (int set_id, int **io_HWCIds)
{
	int i=0, num_counters=0;
	int *HWCIds=NULL;

	num_counters = HWC_sets[set_id].num_counters;
    
	xmalloc(HWCIds, MAX_HWC * sizeof(int));

	for (i=0; i<num_counters; i++)
		HWCIds[i] = HWC_sets[set_id].counters[i];

	for (i=num_counters; i<MAX_HWC; i++)
		HWCIds[i] = NO_COUNTER;

	*io_HWCIds = HWCIds;
	return num_counters;
}

#include "../../merger/paraver/HardwareCounters.h" /* XXX: Include should be moved to common files */
int HWC_Get_Set_Counters_ParaverIds (int set_id, int **io_HWCParaverIds)
{
	int i=0, num_counters=0;
	int *HWCIds=NULL;

	num_counters = HWC_Get_Set_Counters_Ids (set_id, &HWCIds);
	
	/* Convert PAPI/PMAPI Ids to Paraver Ids */
	for (i=0; i<num_counters; i++)
#if defined(PMAPI_COUNTERS)
        HWCIds[i] = HWC_COUNTER_TYPE(i, HWCIds[i]);
#else
        HWCIds[i] = HWC_COUNTER_TYPE(HWCIds[i]);
#endif

    *io_HWCParaverIds = HWCIds;
    return num_counters;
}

/* Returns the index in which is stored the given counter in a set */
int HWC_Get_Position_In_Set (int set_id, int hwc_id)
{
	int i = 0, num_counters = 0;

    num_counters = HWC_sets[set_id].num_counters;

	for (i=0; i<num_counters; i++)
	{
		int cur_hwc_id;
#if defined(PMAPI_COUNTERS)
		cur_hwc_id = HWC_COUNTER_TYPE(i, HWC_sets[set_id].counters[i]);
#else
		cur_hwc_id = HWC_COUNTER_TYPE(HWC_sets[set_id].counters[i]);
#endif
		if (cur_hwc_id == hwc_id) return i;
	}
	return -1;
}

/**
 * Stops the current set and starts reading the next one.
 * \param thread_id The thread that changes the set. 
 */
void HWC_Start_Next_Set (UINT64 time, int thread_id)
{
	/* If there are less than 2 sets, don't do anything! */
	if (HWC_num_sets > 1)
	{
		HWCBE_STOP_SET (time, HWC_current_set[thread_id], thread_id);

		/* Move to the next set */
		HWC_current_set[thread_id] = (HWC_current_set[thread_id] + 1) % HWC_num_sets;

		HWCBE_START_SET (time, HWC_current_set[thread_id], thread_id);
	}
}

/** 
 * Stops the current set and starts reading the previous one.
 * \param thread_id The thread that changes the set.
 */
void HWC_Start_Previous_Set (UINT64 time, int thread_id)
{
	/* If there are less than 2 sets, don't do anything! */
	if (HWC_num_sets > 1)
	{
		HWCBE_STOP_SET (time, HWC_current_set[thread_id], thread_id);

		/* Move to the previous set */
		HWC_current_set[thread_id] = ((HWC_current_set[thread_id] - 1) < 0) ? (HWC_num_sets - 1) : (HWC_current_set[thread_id] - 1) ;

		HWCBE_START_SET (time, HWC_current_set[thread_id], thread_id);
	}
}

/** 
 * Changes the current set for the given thread, depending on the number of global operations.
 * \param count_glops Counts how many global operations have been executed so far 
 * \param time Timestamp where the set change is checked
 * \param thread_id The thread identifier.
 * \return 1 if the set is changed, 0 otherwise.
 */

static inline int CheckForHWCSetChange_GLOPS (unsigned int count_glops, UINT64 time, int thread_id)
{
	if (HWC_current_changeat != 0)
	{
		if (HWC_current_changeat <= count_glops)
		{
			HWC_Start_Next_Set (time, thread_id);
			/* Start_Next_Set initializes HWC_current_changeat to the value set in the XML,
			   so the next change will take place after that number of glops starting from
			   the current number of glops. */
			HWC_current_changeat += count_glops;
			return 1;
		}
	}
	return 0;
}

/** 
 * Changes the current set for the given thread, depending on the time that has passed. 
 * \param time Timestamp where the set change is checked
 * \param thread_id The thread identifier.
 * \return 1 if the set is changed, 0 otherwise.
 */
static inline int CheckForHWCSetChange_TIME (UINT64 time, int threadid)
{
	int ret = 0;
	if (HWC_current_timebegin[threadid] + HWC_current_changeat < time)
	{
		HWC_Start_Next_Set (time, threadid);
		ret = 1;
	}
	return ret;
}

/**
 * Checks for pending set changes of the given thread.
 * \param count_glops Counts how many global operations have been executed so far 
 * \param time Timestamp where the set change is checked
 * \param thread_id The thread identifier.
 * \return 1 if the set is changed, 0 otherwise.
 */
int HWC_Check_Pending_Set_Change (unsigned int count_glops, UINT64 time, int thread_id)
{
	if (HWC_current_changetype == CHANGE_GLOPS)
		return CheckForHWCSetChange_GLOPS(count_glops, time, thread_id);
	else if (HWC_current_changetype == CHANGE_TIME)
		return CheckForHWCSetChange_TIME(time, thread_id);
	else
		return 0;
}

/** 
 * Initializes the hardware counters module.
 * \param options Configuration options.
 */
void HWC_Initialize (int options)
{
	int i, num_threads = Backend_getMaximumOfThreads();

	HWC_current_set = (int *)malloc(sizeof(int) * num_threads);
	if (NULL == HWC_current_set)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Cannot allocate memory for HWC_current_set\n");
		return;
	}

	HWC_current_timebegin = (unsigned long long *)malloc(sizeof(unsigned long long) * num_threads);
	if (NULL == HWC_current_timebegin)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Cannot allocate memory for HWC_current_timebegin\n");
		return;
	}

	for (i = 0; i < num_threads; i++)
	{
		HWC_current_set[i] = 0;
		HWC_current_timebegin[i] = 0;
	}

	HWCBE_INITIALIZE(options);
}

/**
 * Starts reading counters.
 * \param num_threads Total number of threads.
 */
void HWC_Start_Counters (int num_threads)
{
	int i;

	HWC_Thread_Initialized = (int *) malloc (sizeof(int) * num_threads);
	if (NULL == HWC_Thread_Initialized)
	{
		fprintf (stderr, PACKAGE_NAME ": Error! Cannot allocate memory for HWC_Thread_Initialized!\n");
		return;
	}

	/* Mark all the threads as uninitialized */
	for (i = 0; i < num_threads; i++)
	{
		HWC_Thread_Initialized[i] = FALSE;
	}

	Accumulated_HWC_Valid = (int *)malloc(sizeof(int) * num_threads);
	if (NULL == Accumulated_HWC_Valid)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Cannot allocate memory for Accumulated_HWC_Valid\n");
		return;
	}

	Accumulated_HWC = (long long **)malloc(sizeof(long long *) * num_threads);
	if (NULL == Accumulated_HWC)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Cannot allocate memory for Accumulated_HWC\n");
		return;
	}

	for (i = 0; i < num_threads; i++)
	{
		Accumulated_HWC[i] = (long long *)malloc(sizeof(long long) * MAX_HWC);
		if (NULL == Accumulated_HWC[i])
		{
			fprintf (stderr, PACKAGE_NAME": Error! Cannot allocate memory for Accumulated_HWC[%d]\n", i);
			return;
		}
		HWC_Accum_Reset(i);
	}

	if (HWC_num_sets <= 0)
		return;

	HWCEnabled = TRUE;

	/* Init counters for thread 0 */
	HWCEnabled = HWCBE_START_COUNTERS_THREAD (TIME, 0);
}

/** 
 * Starts reading counters for new threads. 
 * \param old_num_threads Previous number of threads.
 * \param new_num_threads New number of threads.
 */
void HWC_Restart_Counters (int old_num_threads, int new_num_threads)
{
	int i;

#if defined(PAPI_COUNTERS)
	for (i = 0; i < HWC_num_sets; i++)
		HWCBE_PAPI_Allocate_eventsets_per_thread (i, old_num_threads, new_num_threads);
#endif

	HWC_Thread_Initialized = (int *) realloc (HWC_Thread_Initialized, sizeof(int) * new_num_threads);
	if (NULL == HWC_Thread_Initialized)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Cannot reallocate memory for HWC_Thread_Initialized!\n");
		return;
	}

	/* Mark all the threads as uninitialized */
	for (i = old_num_threads; i < new_num_threads; i++)
		HWC_Thread_Initialized[i] = FALSE;

	Accumulated_HWC_Valid = (int *) realloc (Accumulated_HWC_Valid, sizeof(int) * new_num_threads);
	if (NULL == Accumulated_HWC_Valid)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Cannot reallocate memory for Accumulated_HWC_Valid\n");
		return;
	}

	Accumulated_HWC = (long long **) realloc (Accumulated_HWC, sizeof(long long *) * new_num_threads);
	if (NULL == Accumulated_HWC)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Cannot reallocate memory for Accumulated_HWC\n");
		return;
	}

	for (i = old_num_threads; i < new_num_threads; i++)
	{
		Accumulated_HWC[i] = (long long *)malloc(sizeof(long long) * MAX_HWC);
		if (NULL == Accumulated_HWC[i])
		{
			fprintf (stderr, PACKAGE_NAME": Error! Cannot reallocate memory for Accumulated_HWC[%d]\n", i);
			return;
		}
		HWC_Accum_Reset(i);
	}

	HWC_current_set = (int *) realloc (HWC_current_set, sizeof(int) * new_num_threads);
	if (NULL == HWC_current_set)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Cannot reallocate memory for HWC_current_set\n");
		return;
	}

	HWC_current_timebegin = (unsigned long long *) realloc (HWC_current_timebegin, sizeof(unsigned long long) * new_num_threads);
	if (NULL == HWC_current_timebegin)
	{
		fprintf (stderr, PACKAGE_NAME": Error! Cannot reallocate memory for HWC_current_timebegin\n");
		return;
	}

	for (i = old_num_threads; i < new_num_threads; i++)
	{
		HWC_current_set[i] = 0;
		HWC_current_timebegin[i] = 0;
	}
}

/**
 * Parses the XML configuration and setups the sets distribution.
 * \param task_id The task identifier.
 * \param num_tasks Total number of tasks.
 * \param distribution The user defined distribution scheme.
 */
void HWC_Parse_XML_Config (int task_id, int num_tasks, char *distribution)
{
	unsigned threadid = 0;

	/* Do this if we have more than 1 counter set */
	if (HWC_num_sets > 1)
	{
		if (strncasecmp (distribution, "cyclic", 6) == 0)
		{
			/* Sets are distributed among tasks like:
			0 1 2 3 .. n-1 0 1 2 3 .. n-1  0 1 2 3 ... */
			for(threadid=0; threadid<Backend_getMaximumOfThreads(); threadid++) 
				HWC_current_set[threadid] = task_id % HWC_num_sets;

			if (task_id == 0)
				fprintf (stdout, PACKAGE_NAME": Starting distribution hardware counters set is established to 'cyclic'\n");
		}
		else if (strncasecmp (distribution, "block", 5) == 0)
		{
			/* Sets are distributed among tasks in a 
			0 0 0 0 .. 1 1 1 1 .... n-1 n-1 n-1 n-1  
			fashion */

			/* a/b rounded to highest is (a+b-1)/b */
			int BlockDivisor = (num_tasks+HWC_num_sets-1) / HWC_num_sets;
			for(threadid=0; threadid<Backend_getMaximumOfThreads(); threadid++) 
			{
				if (BlockDivisor > 0)
					HWC_current_set[threadid] = task_id / BlockDivisor;
				else
					HWC_current_set[threadid] = 0;
			}

			if (task_id == 0)
				fprintf (stdout, PACKAGE_NAME": Starting distribution hardware counters set is established to 'block'\n");
		}
		else
		{
			/* Did the user placed a fixed value? */
			int value = atoi (distribution);
			if (value == 0)
			{
				if (task_id == 0)
					fprintf (stderr, PACKAGE_NAME": Warning! Cannot identify '%s' as a valid starting distribution set on the CPU counters. Setting to the first one.\n", distribution);
				for(threadid=0; threadid<Backend_getMaximumOfThreads(); threadid++)
					HWC_current_set[threadid] = 0;
			}
			else
				for(threadid=0; threadid<Backend_getMaximumOfThreads(); threadid++)
					HWC_current_set[threadid] = (HWC_num_sets<value-1)?HWC_num_sets:value-1;
		}
	}
}

/**
 * Parses the environment variables configuration (intended for executions without XML support).
 * \param task_id The task identifier.
 */
void HWC_Parse_Env_Config (int task_id)
{
    int res, numofcounters;
    char **setofcounters;

    numofcounters = explode (getenv("EXTRAE_COUNTERS"), ",", &setofcounters);
    res = HWC_Add_Set (1, task_id, numofcounters, setofcounters, getenv("EXTRAE_COUNTERS_DOMAIN"), 0, 0, 0, NULL, 0);
}

/** 
 * Reads counters for the given thread and stores the values in the given buffer. 
 * \param tid The thread identifier.
 * \param time When did the event occurred (if so)
 * \param store_buffer Buffer where the counters will be stored.
 * \return 1 if counters were read successfully, 0 otherwise.
 */
int HWC_Read (unsigned int tid, UINT64 time, long long *store_buffer)
{
	int read_ok = FALSE, reset_ok = FALSE; 

	if (HWCEnabled)
	{
		if (!HWC_Thread_Initialized[tid])
			HWCBE_START_COUNTERS_THREAD(time, tid);
		TOUCH_LASTFIELD( store_buffer );

		read_ok = HWCBE_READ (tid, store_buffer);
		reset_ok = (Reset_After_Read ? HWCBE_RESET (tid) : TRUE);
	}
	return (HWCEnabled && read_ok && reset_ok);
}

/**
 * Resets the counters of the given thread.
 * \param tid The thread identifier.
 * \return 1 if success, 0 otherwise.
 */
int HWC_Reset (unsigned int tid)
{
	return ((HWCEnabled) ? HWCBE_RESET (tid) : 0);
}

/**
 * Returns whether counters are reset after reads 
 * \return 1 if counters are reset, 0 otherwise
 */
int HWC_Resetting ()
{
	return Reset_After_Read;
}

/**
 * Accumulates the counters of the given thread in a buffer.
 * \param tid The thread identifier.
 * \param time When did the event occurred (if so)
 * \return 1 if success, 0 otherwise.
 */
int HWC_Accum (unsigned int tid, UINT64 time)
{
	int accum_ok = FALSE; 

	if (HWCEnabled)
	{
		if (!HWC_Thread_Initialized[tid])
			HWCBE_START_COUNTERS_THREAD(time, tid);
		TOUCH_LASTFIELD( Accumulated_HWC[tid] );

#if defined(SAMPLING_SUPPORT)
		/* If sampling is enabled, the counters are always in "accumulate" mode
		   because PAPI_reset is not called */
		accum_ok = HWCBE_READ (tid, Accumulated_HWC[tid]);
#else
		accum_ok = HWCBE_ACCUM (tid, Accumulated_HWC[tid]);
#endif

		Accumulated_HWC_Valid[tid] = TRUE;
	}
	return (HWCEnabled && accum_ok);
}

/**
 * Sets to zero the counters accumulated for the given thread.
 * \param tid The thread identifier.
 * \return 1 if success, 0 otherwise.
 */
int HWC_Accum_Reset (unsigned int tid)
{
	if (HWCEnabled)
	{
		Accumulated_HWC_Valid[tid] = FALSE;
		memset(Accumulated_HWC[tid], 0, MAX_HWC * sizeof(long long));
		return 1;
	}
	else return 0;
}

/** Returns whether Accumulated_HWC contains valid values or not */
int HWC_Accum_Valid_Values (unsigned int tid) 
{
	return ( HWCEnabled ? Accumulated_HWC_Valid[tid] : 0 );
}

/** 
 * Copy the counters accumulated for the given thread to the given buffer.
 * \param tid The thread identifier.
 * \param store_buffer Buffer where the accumulated counters will be copied. 
 */ 
int HWC_Accum_Copy_Here (unsigned int tid, long long *store_buffer)
{
	if (HWCEnabled)
	{
		memcpy(store_buffer, Accumulated_HWC[tid], MAX_HWC * sizeof(long long));
		return 1;
	}
	else return 0;
}

/**
 * Add the counters accumulated for the given thread to the given buffer.
 * \param tid The thread identifier.
 * \param store_buffer Buffer where the accumulated counters will be added. 
 */
int HWC_Accum_Add_Here (unsigned int tid, long long *store_buffer)
{
	int i;
	if (HWCEnabled)
	{
		for (i=0; i<MAX_HWC; i++)
		{
			store_buffer[i] += 	Accumulated_HWC[tid][i];
		}
		return 1;
	}
	else return 0;
}

/**
 * Configures a new set of counters.
 */
int HWC_Add_Set (int pretended_set, int rank, int ncounters, char **counters,
	char *domain, char *change_at_globalops, char *change_at_time,
	int num_overflows, char **overflow_counters, unsigned long long *overflow_values)
{
	return HWCBE_ADD_SET (pretended_set, rank, ncounters, counters, domain, change_at_globalops, change_at_time, num_overflows, overflow_counters, overflow_values);
}

/** 
 * Configures the hwc change time frequency (forces the change_type to CHANGE_TIME!)
 * \param set The HWC set to configure.
 * \param ns The new frequency (in ns).
 */
void HWC_Set_ChangeAtTime_Frequency (int set, unsigned long long ns)
{
	if ((set >= 0) && (set < HWC_Get_Num_Sets()) && (ns > 0))
	{
        HWC_sets[set].change_type = CHANGE_TIME;
		HWC_sets[set].change_at = ns;
	}
	HWC_current_changetype = CHANGE_TIME;
}

int isSamplingEnabled(void)
{
#if defined(SAMPLING_SUPPORT)
	return EnabledSampling;
#else
	return FALSE;
#endif
}

void setSamplingEnabled (int enabled)
{
#if !defined(SAMPLING_SUPPORT)
	UNREFERENCED_PARAMETER(enabled);
#else
	EnabledSampling = (enabled != FALSE);
#endif
}

