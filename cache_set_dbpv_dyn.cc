#include "cache_set_dbpv_dyn.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"
#include "cache.h"
#include "stats.h"

/* Each cache block have been appended with a 1-bit counter
 * which measure if the block has been reused or not. 1 bit
 * is sufficient to find out if the block is dead or not.
 * 
 * Each cache block also contains a 1-bit counter to indicate
 * if the block belongs to a Core 0 or Core 1
*/

#define MAX_BLOCK_COUNT                 1      //(2^1 - 1) 1 bit counter
#define LARGEST_16_BIT_NUMBER           65535  //(2^16 - 1) 16 bit counter
#define TOTAL_NUM_CACHE_BLOCKS          65536  // Number of cache blocks in 4BM cache
#define DB_PERCENT_THRESHOLD_99         9999    
#define DB_PERCENT_THRESHOLD_90         9000    

static UInt8  g_iteration_count         = 0;
static UInt64 g_numTotalDeadBlocksC0    = 0;
static UInt64 g_numTotalBlocksInsC0     = 0;
static UInt64 g_numTotalDeadBlocksC1    = 0;
static UInt64 g_numTotalBlocksInsC1     = 0;
static UInt64 g_numBlocksInvalid        = 0;
static UInt64 g_numTieAtEvict           = 0;
static UInt64 g_numPhases               = 0;

static UInt32 g_phaseID                 = 0;

#define NUM_PHASES 100

/* To find the number of accesses to each block accessed */
static UInt64  g_block_access_count[NUM_PHASES][10];


/* For each phase, i want to record the access trace
 * (Sampling based deadblock prediction), number of unique blocks
 * accessed each phase and number of hits they get, hit rate in the
 * phase, time information of each block (for how many cycles the cache
 * block is in cache, MPKI and Branch misprediction rate in each phase
 * IPC and actual deadblock information
 *
 * Number of unique blocks and their access counts
 * To find number of blocks which are accessed uniquely, we can find blocks
 * which are not dead. So, on eviction we can find blocks which are not dead
 * and count their numbers and their access counts.
 */ 

/* These are 16 bit counters */
static UInt32 g_ValidDeadBlocksC0    = 0;
static UInt32 g_InsValidBlocksC0     = 0;
static UInt32 g_ValidDeadBlocksC1    = 0;
static UInt32 g_InsValidBlocksC1     = 0;

static UInt8 g_core0_insert, g_core1_insert;

static UInt8  g_insertionCore0[1024];
static UInt8  g_insertionCore1[1024];
static UInt16 g_iteration = 0;

CacheSetDBPV_DYN::CacheSetDBPV_DYN(
      String cfgname, core_id_t core_id,
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info, UInt8 num_attempts)
   : CacheSet(cache_type, associativity, blocksize)
   , m_saturation_counter_max_value(Sim()->getCfg()->getIntArray(cfgname + "/srrip/max_value", core_id))
   , m_db_percent_threshold(Sim()->getCfg()->getIntArray(cfgname + "/srrip/db_threshold", core_id))
   , m_rrip_numbits(Sim()->getCfg()->getIntArray(cfgname + "/srrip/bits", core_id))
   , m_rrip_max((1 << m_rrip_numbits) - 1)
   , m_rrip_insert(m_rrip_max - 1)
   , m_replacement_pointer(0)
   , m_set_info(set_info)
{
   m_rrip_bits = new UInt8[m_associativity];
   for (UInt32 i = 0; i < m_associativity; i++)
      m_rrip_bits[i] = m_rrip_insert;

    /* To record how many times a block got hit */
    m_block_access = new UInt8[m_associativity];
    for (UInt8 i = 0; i < m_associativity; i++)
    {
        m_block_access[i] = 0;
    }
    
    /* To record which core brought the block into cache */
    m_block_owner = new UInt8[m_associativity];
    for (UInt8 i = 0; i < m_associativity; i++)
    {
        m_block_owner[i] = 0;
    }

    if (0 == g_iteration_count)
    {
        g_iteration_count++;

        printf("\n[Newton] DBPV_DYN with associativity:%d Counter Limit:%u DB Threshold:%u!!!\n",
                m_associativity, m_saturation_counter_max_value, m_db_percent_threshold);
        registerStatsMetric("interval_timer", core_id, "totalBlocksDeadC0", &g_numTotalDeadBlocksC0);
        registerStatsMetric("interval_timer", core_id, "totalBlocksInsC0",  &g_numTotalBlocksInsC0);

        registerStatsMetric("interval_timer", core_id, "totalBlocksDeadC1", &g_numTotalDeadBlocksC1);
        registerStatsMetric("interval_timer", core_id, "totalBlocksInsC1",  &g_numTotalBlocksInsC1);

        registerStatsMetric("interval_timer", core_id, "InvalidBlocks",     &g_numBlocksInvalid);
        registerStatsMetric("interval_timer", core_id, "NumTieAtEvict",     &g_numTieAtEvict);
        registerStatsMetric("interval_timer", core_id, "numPhases",         &g_numPhases);
        
        /* Initialize the insertion locations */
        g_core1_insert = m_rrip_insert;
        g_core0_insert = m_rrip_insert;
        
       /* To record information about number of blocks having a particular
        * reference count */
       for (UInt32 g_phaseID = 0; g_phaseID < NUM_PHASES; g_phaseID++)
       {
          for (UInt32 i = 0; i < 5; i++)
          {
             g_block_access_count[g_phaseID][i] = 0;
             registerStatsMetric("interval_timer", core_id,
                                 String("dbpv_block-access-count-")+itostr(g_phaseID)+"-"+itostr(i),
                                 &g_block_access_count[g_phaseID][i]);
          }
       }
       
       printf("PhaseID in progress:%u\n", g_phaseID);
       g_numPhases = g_phaseID;
    }
}

CacheSetDBPV_DYN::~CacheSetDBPV_DYN()
{
   delete [] m_rrip_bits;
   
   for (UInt32 i = 0; i < g_iteration; i++)
   {
        printf("C0:%u C1:%u\n", g_insertionCore0[i], g_insertionCore1[i]);
   }
}

static void checkForRRIPTie(UInt8 *m_rrip_bits, UInt8 m_rrip_max, UInt8 m_associativity)
{
    UInt8 numEntriesTieArray = 0;
    
    for (UInt32 i = 0; i < m_associativity; i++)
    {
        if (m_rrip_bits[i] == m_rrip_max)
        {
            numEntriesTieArray++;
        }
    }
    
    if (numEntriesTieArray > 1)
    {
        /* It means that a tie has occured between RRIP values */
         g_numTieAtEvict++;
    }
}

/* This function should update the values of insertion of blocks of
 * Core 0 and Core 1 on the basis of SDM, In case the number of dead blocks for
 * more than 95% and miss rate is also more than 95%, the blocks for that core
 * are assumed to be streaming and inserted at LRU position
 */
void UpdateBlockInsertionLocation(UInt32 m_saturation_counter_max_value,
                                  UInt32 m_db_percent_threshold,  
                                  UInt8 m_rrip_max, UInt8 m_rrip_insert, UInt8 coreID)
{
    UInt32 db_percent_c0 = 0;
    UInt32 db_percent_c1 = 0;

    printf("\nUpdatingInsertionLocation:InvBlks=%lu", g_numBlocksInvalid);

    printf("\nDT_c0:%lu, DV_c0:%u, InT_c0:%lu, InV_c0:%u, DT_c1:%lu, DV_c1:%u, InT_c1:%lu, InV_c1:%u",
           g_numTotalDeadBlocksC0, g_ValidDeadBlocksC0, g_numTotalBlocksInsC0, g_InsValidBlocksC0,
           g_numTotalDeadBlocksC1, g_ValidDeadBlocksC1, g_numTotalBlocksInsC1, g_InsValidBlocksC1);

    /* Check if all the cache lines have been filled, cache has been warmed */
    /* Calculate the percent of dead blocks for each application */
    if (0 == coreID)
    {
        if (g_InsValidBlocksC0 == m_saturation_counter_max_value)
        {
            db_percent_c0 = (10000 * g_ValidDeadBlocksC0 / g_InsValidBlocksC0);
            printf("\nDeadBlockC0_Per:%d", db_percent_c0);
        }
        
        if (db_percent_c0 >= m_db_percent_threshold)
        {
            g_core0_insert = m_rrip_max;
            printf("\nCore0InsertedAt:%d", m_rrip_max);

            /* If core 1 is already at RRIP MAX, then insert core 0 at 2 even if it
             * has deadblocks more than threshold, so that atleast core 0 is able to
             * utilize cache */
            g_core0_insert = (g_core1_insert == m_rrip_max) ? m_rrip_insert : m_rrip_max;
        }
        else
        {
             printf("\nReverting C0 Back to RRIP 2");
             g_core0_insert = m_rrip_insert;
        }
    }
    
    if (1 == coreID)
    {
        if (g_InsValidBlocksC1 == m_saturation_counter_max_value)
        {
            db_percent_c1 = (10000 * g_ValidDeadBlocksC1 / g_InsValidBlocksC1);
            printf("\nDeadBlockC1_Per:%d", db_percent_c1);
        }
    
        if (db_percent_c1 >= m_db_percent_threshold)
        {
            g_core1_insert = m_rrip_max;
            printf("\nCore1InsertedAt:%d", m_rrip_max);
        
            g_core1_insert = (g_core0_insert == m_rrip_max) ? m_rrip_insert : m_rrip_max;
        }
        else
        {
             printf("\nReverting C1 Back to RRIP 2");
             g_core1_insert = m_rrip_insert;
        }
    }

    /* It has been observed that if one of the application has more than 90%
     * deadblocks, then it is better to insert it at LRU position, i.e. RRIP 3
     * However, if both the applications have more than 90% deadblock, application
     * having more % of deadblock, should be inserted at LRU e.g. in mix of gcc and libq
     * gcc should be at LRU -1 and libq should be at LRU
     */

    printf("\nID:%u DB_Percent => C0:%u C1:%u InsertionLocations => C0:%u C1:%u\n",
            g_phaseID, db_percent_c0, db_percent_c1, g_core0_insert, g_core1_insert);

    g_insertionCore0[g_iteration] = g_core0_insert;
    g_insertionCore1[g_iteration] = g_core1_insert;
}
    
UInt32
CacheSetDBPV_DYN::InsertBlockAtIndex(UInt32 index, core_id_t core_id)
{
    /* When we found a victim block, we are finding how many blocks have
     * same RRPV value
     */
    checkForRRIPTie(m_rrip_bits, m_rrip_max, m_associativity);

    m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;

    LOG_ASSERT_ERROR(isValidReplacement(index), "SRRIP selected an invalid replacement candidate");

    /* Increment the number of access a block gets. We are trying to figure out
     * how many blocks are getting a particular access */
    {
        UInt32 a = m_block_access[index];

        /* When a >= 10, we are storing the accesses in 10 */
        if (a >= 4)
        {
            a = 4;
        }
         
        g_block_access_count[g_phaseID][a]++;
    }

    /* Find if the victim block is dead blocks */
    if (0 == m_block_access[index])
    {
        /* Block is dead, findout who was its owner */
        (0 == m_block_owner[index]) ? g_numTotalDeadBlocksC0++ : g_numTotalDeadBlocksC1++;
        (0 == m_block_owner[index]) ? g_ValidDeadBlocksC0++  : g_ValidDeadBlocksC1++;
    }

    /* Prepare way for a new line: set prediction to 'long' */
    if (core_id == 0)
    {
        m_rrip_bits[index] = g_core0_insert;
        g_numTotalBlocksInsC0++;
        g_InsValidBlocksC0++;
    }
    else if (core_id == 1)
    {
        m_rrip_bits[index] = g_core1_insert;
        g_numTotalBlocksInsC1++;
        g_InsValidBlocksC1++;
    }
    else
    {
        printf("\n\n\n[Newton]ERROR!!!!\n\n\n");
    }
    
    /* Reset its access counters */
    m_block_access[index] = 0;
    m_block_owner[index] = core_id;

    if (g_InsValidBlocksC0 == m_saturation_counter_max_value)
     printf("\nID:%u InsertedC0:%d, DeadC0:%d", g_phaseID, g_InsValidBlocksC0, g_ValidDeadBlocksC0);   

    if (g_InsValidBlocksC1 == m_saturation_counter_max_value)
     printf("\nID:%u InsertedC1:%d, DeadC1:%d", g_phaseID, g_InsValidBlocksC1, g_ValidDeadBlocksC1);   

    if (m_saturation_counter_max_value == g_InsValidBlocksC1)
    {
        UpdateBlockInsertionLocation(m_saturation_counter_max_value,
                                     m_db_percent_threshold, m_rrip_max, m_rrip_insert, 1);
        /* To get more accurate data about phase-wise deadblock percentage, resetting
         * to zero will be fine */
        g_InsValidBlocksC1  = 0;
        g_ValidDeadBlocksC1 = 0;
        g_phaseID++;
        printf("PhaseID in progress:%u\n", g_phaseID);
    }

    if (m_saturation_counter_max_value == g_InsValidBlocksC0)
    {
        UpdateBlockInsertionLocation(m_saturation_counter_max_value,
                                     m_db_percent_threshold, m_rrip_max, m_rrip_insert, 0);
        g_InsValidBlocksC0  = 0;
        g_ValidDeadBlocksC0 = 0;
        g_phaseID++;
        printf("PhaseID in progress:%u\n", g_phaseID);
    }
    
    g_numPhases = g_phaseID;

    #if 0
    printf("\nDT_c0:%lu, DV_c0:%u, InT_c0:%lu, InV_c0:%u, DT_c1:%lu, DV_c1:%u, InT_c1:%lu, InV_c1:%u, coreID:%d",
           g_numTotalDeadBlocksC0, g_ValidDeadBlocksC0, g_numTotalBlocksInsC0, g_InsValidBlocksC0,
           g_numTotalDeadBlocksC1, g_ValidDeadBlocksC1, g_numTotalBlocksInsC1, g_InsValidBlocksC1,
           core_id);
    #endif
    
    return index;
}


UInt32
CacheSetDBPV_DYN::getReplacementIndex(CacheCntlr *cntlr, core_id_t core_id)
{
    for (UInt32 i = 0; i < m_associativity; i++)
    {
        if (!m_cache_block_info_array[i]->isValid())
        {
            /* If there is an invalid line(s) in the set, regardless of the LRU bits
             * of other lines, we choose the first invalid line to replace
             * Prepare way for a new line: set prediction to 'long'
             */
            if (core_id == 0)
            {
                m_rrip_bits[i] = g_core0_insert;
                g_numTotalBlocksInsC0++;
            }
            else if (core_id == 1)
            {
                m_rrip_bits[i] = g_core1_insert;
                g_numTotalBlocksInsC1++;
            }
            else
            {
                printf("\n\n\n[Newton]ERROR!!!!\n\n\n");
            }

            /* Reset its access counters */
            m_block_access[i] = 0;
            m_block_owner[i]  = core_id;
            
            g_numBlocksInvalid++;
         
            return i;
        }
    }

   for(UInt32 j = 0; j <= m_rrip_max; ++j)
   {
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         if (m_rrip_bits[m_replacement_pointer] >= m_rrip_max)
         {
            /* We choose the first non-touched line as the victim (note that we
             * start searching from the replacement pointer position)
             */
            return InsertBlockAtIndex(m_replacement_pointer, core_id);
         }

         m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
     }

      /* Increment all RRIP counters until one hits RRIP_MAX */
      for (UInt32 i = 0; i < m_associativity; i++)
      {
         if (m_rrip_bits[i] < m_rrip_max)
         {
            m_rrip_bits[i]++;
         }
      }
   }

   LOG_PRINT_ERROR("Error finding replacement index");
}

void
CacheSetDBPV_DYN::updateReplacementIndex(UInt32 accessed_index)
{
    /* If block access count have reached saturation limit MAX_BLOCK_COUNT,
     * keep the counter saturated.
     */
    if (MAX_BLOCK_COUNT != m_block_access[accessed_index])
    {
        m_block_access[accessed_index]++;
    }
    
   /* As per SRRIP paper, SRRIP-HP performs better than SRRIP-FP, hence
    * setting the RRPV values directly to 0 is more beneficial than
    * decreasing it slowly.
    */ 
    if (m_rrip_bits[accessed_index] > 0)
    {
        m_rrip_bits[accessed_index] = 0;
    }
}


