#include "cache_set_dbpv.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"
#include "cache.h"
#include "stats.h"


/* S-RRIP: Static Re-reference Interval Prediction policy
 * High Performance Cache Replacement Using Re-Reference Interval Prediction (RRIP)
 * Aamer Jaleel et. al. ISCA2010
 */
#define MAX_BLOCK_COUNT 3 //(2^2 - 1) 2 bit counter

static UInt8  g_iteration_count         = 0;
static UInt64 g_numTotalDeadBlocksC0    = 0;
static UInt64 g_numTotalBlocksInsC0     = 0;
static UInt64 g_numTotalDeadBlocksC1    = 0;
static UInt64 g_numTotalBlocksInsC1     = 0;
static UInt64 g_numBlocksInvalid        = 0;
static UInt64 g_numBlocksReusedOnceC0     = 0;
static UInt64 g_numBlocksReusedOnceC1     = 0;
static UInt64 g_numBlocksReusedTwiceC0    = 0;
static UInt64 g_numBlocksReusedTwiceC1    = 0;    
static UInt64 g_numBlocksReusedThriceOrMoreC1 = 0;
static UInt64 g_numBlocksReusedThriceOrMoreC0 = 0;

CacheSetDBPV::CacheSetDBPV(
      String cfgname, core_id_t core_id,
      CacheBase::cache_t cache_type,
      UInt32 associativity, UInt32 blocksize, CacheSetInfoLRU* set_info, UInt8 num_attempts)
   : CacheSet(cache_type, associativity, blocksize)
   , m_rrip_numbits(Sim()->getCfg()->getIntArray(cfgname + "/srrip/bits", core_id))
   , m_rrip_max((1 << m_rrip_numbits) - 1)
   , m_rrip_insert(m_rrip_max - 1)
   , m_num_attempts(num_attempts)
   , m_replacement_pointer(0)
   , m_case(Sim()->getCfg()->getIntArray(cfgname + "/srrip/case", core_id))
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

        printf("\n[Newton] DBPV with associativity:%d Case:%d!!!\n", m_associativity, m_case);
        registerStatsMetric("interval_timer", core_id, "totalBlocksDeadC0", &g_numTotalDeadBlocksC0);
        registerStatsMetric("interval_timer", core_id, "totalBlocksReusedOnceC0", &g_numBlocksReusedOnceC0);
        registerStatsMetric("interval_timer", core_id, "totalBlocksReusedTwiceC0", &g_numBlocksReusedTwiceC0);
        registerStatsMetric("interval_timer", core_id, "totalBlocksReusedThriceOrMoreC0", &g_numBlocksReusedThriceOrMoreC0);
        registerStatsMetric("interval_timer", core_id, "totalBlocksInsC0",  &g_numTotalBlocksInsC0);

        registerStatsMetric("interval_timer", core_id, "totalBlocksDeadC1", &g_numTotalDeadBlocksC1);
        registerStatsMetric("interval_timer", core_id, "totalBlocksReusedOnceC1", &g_numBlocksReusedOnceC1);
        registerStatsMetric("interval_timer", core_id, "totalBlocksReusedTwiceC1", &g_numBlocksReusedTwiceC1);
        registerStatsMetric("interval_timer", core_id, "totalBlocksReusedThriceOrMoreC1", &g_numBlocksReusedThriceOrMoreC1);
        registerStatsMetric("interval_timer", core_id, "totalBlocksInsC1",  &g_numTotalBlocksInsC1);

        registerStatsMetric("interval_timer", core_id, "InvalidBlocks",     &g_numBlocksInvalid);
    }
}

CacheSetDBPV::~CacheSetDBPV()
{
   delete [] m_rrip_bits;
}

UInt32
CacheSetDBPV::getReplacementIndex(CacheCntlr *cntlr,core_id_t core_id)
{
    UInt8 core0_insert = m_rrip_insert, core1_insert = m_rrip_insert;
    
    switch (m_case)
    {
        case 1: core0_insert = 0; core1_insert = 0; break; //MRU
        case 2: core0_insert = 1; core1_insert = 1; break;
        case 3: core0_insert = 2; core1_insert = 2; break; //SRRIP
        case 4: core0_insert = 3; core1_insert = 3; break; //LRU
        case 5: core0_insert = 1; core1_insert = 2; break; //
        case 6: core0_insert = 2; core1_insert = 1; break;
        case 7: core0_insert = 1; core1_insert = 3; break;
        case 8: core0_insert = 3; core1_insert = 1; break;
        case 9: core0_insert = 0; core1_insert = 3; break;
        case 10: core0_insert = 3; core1_insert = 0; break;
        case 11: core0_insert = 3; core1_insert = 2; break;
        case 12: core0_insert = 2; core1_insert = 3; break;
    }
    
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
                m_rrip_bits[i] = core0_insert;
                g_numTotalBlocksInsC0++;
            }
            else if (core_id == 1)
            {
                m_rrip_bits[i] = core1_insert;
                g_numTotalBlocksInsC1++;
            }
            else
            {
                printf("\n\n\n[Newton]ERROR!!!!\n\n\n");
            }

            /* Reset its access counters */
            m_block_access[i] = 0;
            m_block_owner[i] = core_id;
            
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
            UInt8 index = m_replacement_pointer;

            m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;

            LOG_ASSERT_ERROR(isValidReplacement(index), "SRRIP selected an invalid replacement candidate");

            /* If the block was never accessed more than once, it is dead */
            switch (m_block_access[index])
            {
                case 0: 
                {
                    if (0 == m_block_owner[index])
                    {
                        g_numTotalDeadBlocksC0++;
                    }
                    else if (1 == m_block_owner[index])
                    {
                        g_numTotalDeadBlocksC1++;
                    }
                    else
                    {
                        printf("\n\n\n[Newton] DeadBLock0: CoreInfo ERROR!!!!\n\n\n");
                    }
                    
                    break;
                }

                case 1: 
                {
                    if (0 == m_block_owner[index])
                    {
                        g_numBlocksReusedOnceC0++;
                    }
                    else if (1 == m_block_owner[index])
                    {
                        g_numBlocksReusedOnceC1++;
                    }
                    else
                    {
                        printf("\n\n\n[Newton] DeadBLock1: CoreInfo ERROR!!!!\n\n\n");
                    }

                    break;
                }

                case 2:
                {
                    if (0 == m_block_owner[index])
                    {
                        g_numBlocksReusedTwiceC0++;
                    }
                    else if (1 == m_block_owner[index])
                    {
                        g_numBlocksReusedTwiceC1++;
                    }
                    else
                    {
                        printf("\n\n\n[Newton] DeadBLock2: CoreInfo ERROR!!!!\n\n\n");
                    }

                    break;
                }

                case 3:
                {
                    if (0 == m_block_owner[index])
                    {
                        g_numBlocksReusedThriceOrMoreC0++;
                    }
                    else if (1 == m_block_owner[index])
                    {
                        g_numBlocksReusedThriceOrMoreC1++;
                    }
                    else
                    {
                        printf("\n\n\n[Newton] DeadBLock3: CoreInfo ERROR!!!!\n\n\n");
                    }

                    break;
                }
                
                default:
                printf("\n\n\n[Newton] Default: CoreInfo ERROR %d!!!!\n\n\n", m_block_access[index]);
            }
                

            /* Prepare way for a new line: set prediction to 'long' */
            if (core_id == 0)
            {
                m_rrip_bits[index] = core0_insert;
                g_numTotalBlocksInsC0++;
            }
            else if (core_id == 1)
            {
                m_rrip_bits[index] = core1_insert;
                g_numTotalBlocksInsC1++;
            }
            else
            {
                printf("\n\n\n[Newton]ERROR!!!!\n\n\n");
            }
            
            /* Reset its access counters */
            m_block_access[index] = 0;
            m_block_owner[index] = core_id;

            return index;
         }

         m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
     }

      // Increment all RRIP counters until one hits RRIP_MAX
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
CacheSetDBPV::updateReplacementIndex(UInt32 accessed_index)
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


