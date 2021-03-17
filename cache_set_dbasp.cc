#include "cache_set_dbasp.h"
#include "simulator.h"
#include "config.hpp"
#include "log.h"
#include "cache.h"
#include "stats.h"

extern UInt64 g_instruction_count;
extern UInt64 g_cycles_count;

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
static UInt64 g_numTotalBlocksHitC0     = 0;
static UInt64 g_numTotalBlocksHitC1     = 0;

/* I wanted to implement the UCP: Utility based cache partitioning algorithm.
 * As per my understanding of the algo, we need to define counters for 
 * MRU to LRU recency positions and then increment the corresponding recency
 * counters on re-reference to a particular position.
 * e.g. if the MRU recency position is hit, we will increment the MRU counter,
 * if (MRU - 2) position is hit, we will increment the counter corresponding
 * to (MRU - 2)th position.
 * From the bits in the used to track the position of a block in the recency
 * stack. From the bits we can find which recency counter we need to increment.
 */ 
 
/* 16 recency counters to track the recency counts. These counters are not
 * block based, but are application based. */
static UInt64 g_recencyCounterC0[16];
static UInt64 g_recencyCounterC1[16];

/* These are 16 bit counters */
static UInt32 g_ValidDeadBlocksC0    = 0;
static UInt32 g_InsValidBlocksC0    = 0;
static UInt32 g_ValidDeadBlocksC1    = 0;
static UInt32 g_InsValidBlocksC1    = 0;

static UInt8 g_core0_insert, g_core1_insert;

static UInt8  g_insertionCore0[1024];
static UInt8  g_insertionCore1[1024];
static UInt16 g_iteration = 0;

static UInt8 ways_C0, ways_C1;

CacheSetDBASP::CacheSetDBASP(
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
   static UInt32 setID = 0;
   
   m_setID = setID++;
   
   m_rrip_bits = new UInt8[m_associativity];
   for (UInt32 i = 0; i < m_associativity; i++)
      m_rrip_bits[i] = m_rrip_insert + 5; //Invalid rrip

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

        printf("\n[Newton] DBASP with associativity:%d Counter Limit:%u DB Threshold:%u!!!\n",
                m_associativity, m_saturation_counter_max_value, m_db_percent_threshold);
        registerStatsMetric("interval_timer", core_id, "totalBlocksDeadC0", &g_numTotalDeadBlocksC0);
        registerStatsMetric("interval_timer", core_id, "totalBlocksInsC0",  &g_numTotalBlocksInsC0);

        registerStatsMetric("interval_timer", core_id, "totalBlocksDeadC1", &g_numTotalDeadBlocksC1);
        registerStatsMetric("interval_timer", core_id, "totalBlocksInsC1",  &g_numTotalBlocksInsC1);

        registerStatsMetric("interval_timer", core_id, "totalBlocksHitC0",  &g_numTotalBlocksHitC0);
        registerStatsMetric("interval_timer", core_id, "totalBlocksHitC1",  &g_numTotalBlocksHitC1);


        registerStatsMetric("interval_timer", core_id, "InvalidBlocks",     &g_numBlocksInvalid);
        
        /* Initialize the insertion locations: MRU position */
        g_core0_insert = m_rrip_insert;
        g_core1_insert = m_rrip_insert;

        ways_C1 = ways_C0 = m_associativity/2;
        
       for(UInt32 i = 0; i < m_associativity; i++)
       {
          g_recencyCounterC0[i] = 0;
          registerStatsMetric("interval_timer", core_id,
                              String("recencyCounterC0-")+itostr(i), &g_recencyCounterC0[i]);

          registerStatsMetric("interval_timer", core_id,
                              String("recencyCounterC1-")+itostr(i), &g_recencyCounterC1[i]);
       }
    }
}

CacheSetDBASP::~CacheSetDBASP()
{
   delete [] m_rrip_bits;
   
   for (UInt32 i = 0; i < g_iteration; i++)
   {
        printf("C0:%u C1:%u\n", g_insertionCore0[i], g_insertionCore1[i]);
   }
}

static void UCPpartition()
{
    int associativity = 16;
 
    int totalAccessC0 = g_numTotalBlocksInsC0 + g_numTotalBlocksHitC0;
    int totalAccessC1 = g_numTotalBlocksInsC1 + g_numTotalBlocksHitC1;

    int miss_c0[17];
    int miss_c1[17];
    int hit_c0[17];
    int hit_c1[17];
    int utility[17];
    int hit0, hit1, i, j, max, max_i;
    
    miss_c0[0] = totalAccessC0;
    miss_c1[0] = totalAccessC1;

    hit0 = hit1 = 0;
    
    for (i = 0; i < associativity; i++)
    {
        hit0 += g_recencyCounterC0[i];
        hit1 += g_recencyCounterC1[i];
        
        hit_c0[i+1] = hit0;
        hit_c1[i+1] = hit1;

        /* miss_c0[1]  contains misses when 1  way is   given, similarly
         * miss_c0[16] contains misses when 16 ways are given */

        miss_c0[i+1] = totalAccessC0 - hit_c0[i+1];
        miss_c1[i+1] = totalAccessC1 - hit_c1[i+1];
        
        printf("\ni=%d totalAccC0:%d totalAccC1:%d hitC0:%d hitC1:%d missC0=%d missC1=%d", i+1, totalAccessC0, totalAccessC1, hit_c0[i+1], hit_c1[i+1], miss_c0[i+1], miss_c1[i+1]);
    }

    /* Recording utility for each combination of way distribution */
    for (j = 1; j < associativity; j++)
    {
        utility[j] = (miss_c0[0] - miss_c0[j]) + (miss_c1[0] - miss_c1[16-j]);
        printf("\nj:%d Util:%d C0: miss0:%d missj:%d C1: miss0:%d missj:%d", j, utility[j], miss_c0[0], miss_c0[j], miss_c1[0], miss_c1[16-j]);
    }

    max = utility[1];
    max_i = 1;
 
    /* We need to find the maximum utility */
    for (i = 2; i < associativity; i++)
    {
        if (utility[i] > max)
        {
            max_i = i;
            max = utility[i];
            printf("\nMax_i:%d maxUtil:%d", max_i, max);
        }
    }
    
    printf("\nMax_i:%d maxUtil:%d\n", max_i, max);

    ways_C0 = max_i;
    ways_C1 = associativity - ways_C0;
    printf("\n[Newton] Utility Changed C0:%u, C1:%u", ways_C0, ways_C1);
}

static UInt32 getLRUCandidate(UInt32 *pArr, UInt32 entries)
{
    UInt32 max = pArr[0];
    UInt32 max_i = 0, i;
 
    //#if 0
    printf("\n");
    for (i = 0; i < entries; i++)
    {
        printf("%u:%u  ",i,pArr[i]);
    }
    printf("\n");
    //#endif
 
    /* We need to find the maximum utility */
    for (i = 1; i < entries; i++)
    {
        //printf("\n%u:%u  ",i,pArr[i]);
        if (pArr[i] > max)
        {
            max_i = i;
            max = pArr[i];
            printf("\nMax_i:%d maxUtil:%d\n", max_i, max);
        }
    }
    
    printf("\nnumEntries:%u Max_i:%d maxUtil:%d\n", entries, max_i, max);
    
    return max_i;
}


/* when the block will be inserted at MRU position, all the blocks will
 * shift in the LRU recency stack by one
 */
static void insertBlockToNearToLRUposition(UInt32 uiIndex, UInt8 *p_rrip_bits,
                                    UInt32 associativity, UInt32 rrpv)
{
    /* To insert a block at (LRU - 1) position in the recency stack, we need to
     * increment the recency of (LRU - 1) block to LRU and then insert the block
     * at (LRU - 1)
     */
    for(UInt32 i = 0; i < associativity; i++)
    {
        /* Finding the block at (LRU - 1) and move it to LRU */
        if (p_rrip_bits[i] == rrpv - 1)
        {
            p_rrip_bits[i]++;

            /* I assume only one block/line to be at (LRU - 1) */
            printf("\nDemoted Block@Index:%u to LRU\n", i);
            break;
        }
    }

    p_rrip_bits[uiIndex] = rrpv - 1;
   
    //#if 0
    for (UInt32 i = 0; i < associativity;  i++)
    {
        printf("%u:%u  ", i, p_rrip_bits[i]);
    } 

    printf("\nRecencyCtr\n");
    for(UInt32 i = 0; i < associativity; i++)
    {
       printf("%u:%lu  ", i, g_recencyCounterC0[i]); 
    }
    //#endif
}

/* when the block will be inserted at MRU position, all the blocks will
 * shift in the LRU recency stack by one
 */
static void insertBlockToMRUposition(UInt32 uiIndex, UInt8 *p_rrip_bits, UInt32 associativity)
{
    for(UInt32 i = 0; i < associativity; i++)
    {
        if (p_rrip_bits[i] < p_rrip_bits[uiIndex])
            p_rrip_bits[i]++;
    }

    p_rrip_bits[uiIndex] = 0;
   
    //if 0
    for (UInt32 i = 0; i < associativity;  i++)
    {
        printf("%u:%u  ", i, p_rrip_bits[i]);
    } 

    printf("\nRecencyCtr\n");
    for(UInt32 i = 0; i < associativity; i++)
    {
       printf("%u:%lu  ", i, g_recencyCounterC0[i]); 
    }
    //#endif
}

UInt32
CacheSetDBASP::InsertBlockAtIndex(UInt32 index, core_id_t core_id)
{
    /* When we found a victim block, we are finding how many blocks have
     * same RRPV value
     */
    m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;

    LOG_ASSERT_ERROR(isValidReplacement(index), "SRRIP selected an invalid replacement candidate");

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
        //m_rrip_bits[index] = g_core0_insert;
        g_numTotalBlocksInsC0++;
        g_InsValidBlocksC0++;
    }
    else if (core_id == 1)
    {
        //m_rrip_bits[index] = g_core1_insert;
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

    printf("\nEviction at Index:%u rrpv%u\n", index, m_rrip_bits[index]);
    insertBlockToNearToLRUposition(index, m_rrip_bits, m_associativity, m_rrip_bits[index]);
     
    return index;
}

UInt32
CacheSetDBASP::getReplacementIndex(CacheCntlr *cntlr, core_id_t core_id)
{
    static UInt64 million_cycle_count = 1;
    
    printf("\nEviction: SetID=%u\n", m_setID);
    
    if (g_cycles_count / (1000000 * million_cycle_count))
    {
        printf("\n[Newton] UCP called %lu times @ %lu", million_cycle_count, g_cycles_count);
        /* Calling partitioning function after */
        UCPpartition();

        million_cycle_count = g_cycles_count / 1000000;
        printf("\nMillionCycleCnt:%lu", million_cycle_count);                

        million_cycle_count++;
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
                //m_rrip_bits[i] = g_core0_insert;
                g_numTotalBlocksInsC0++;
            }
            else if (core_id == 1)
            {
                //m_rrip_bits[i] = g_core1_insert;
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
         
            printf("\nEviction at Index:%u\n", i);
            insertBlockToNearToLRUposition(i, m_rrip_bits, m_associativity, m_rrip_max);
	        return i;
        }
    }

        
    {

        UInt32 rrip_blocksC0[16];
        UInt32 rrip_blocksC1[16];
        UInt32 blockC0_index[16];
        UInt32 blockC1_index[16];
        
        UInt32 num_rrip_blocksC0 = 0;
        UInt32 num_rrip_blocksC1 = 0;
        UInt32 eviction_C0 = 0;
        UInt32 eviction_C1 = 0;

        for (UInt32 i = 0; i < m_associativity; i++)
        {
            if (0 == m_block_owner[i])
            {
                rrip_blocksC0[num_rrip_blocksC0] = m_rrip_bits[i];
                blockC0_index[num_rrip_blocksC0] = i;    
                num_rrip_blocksC0++;
            }
            else if (1 == m_block_owner[i])
            {
                rrip_blocksC1[num_rrip_blocksC1] = m_rrip_bits[i];
                blockC1_index[num_rrip_blocksC1] = i;    
                num_rrip_blocksC1++;
            }
        }
            
        eviction_C0 = getLRUCandidate(rrip_blocksC0, num_rrip_blocksC0);
        eviction_C1 = getLRUCandidate(rrip_blocksC1, num_rrip_blocksC1);

        /* To find true eviction index in the entire LRU stack: 16 ways */
        eviction_C0 = blockC0_index[eviction_C0];
        eviction_C1 = blockC1_index[eviction_C1];
  
        if ((0 !=  m_block_owner[eviction_C0]) || (1 !=  m_block_owner[eviction_C1]))
        {
            LOG_PRINT_ERROR("WrongOwner");
        }
       
        printf("\nWays C0:%u C1:%u block C0:%u C1:%u", ways_C0, ways_C1, num_rrip_blocksC0, num_rrip_blocksC1);

        if (ways_C0 > num_rrip_blocksC0)
        {
           /* The number of ways allocated to core0 is more than what
            * is currently allocated, so we need to evict LRU block from C1 */
            m_replacement_pointer = eviction_C1;
            printf("\nCore1 block evicted:%u", eviction_C1);
        }
        else if (ways_C0 < num_rrip_blocksC0)
        {
            m_replacement_pointer = eviction_C0;
            printf("\nCore0 block evicted:%u", eviction_C0);
        }
        else if (ways_C0 == num_rrip_blocksC0)
        {
            /* If ways are equal to alloted quota, evict C0 block if new
             * block to be inserted is from C0 and C1 if new block is of C1
             */
            m_replacement_pointer = (0 == core_id) ? eviction_C0 : eviction_C1;
        } 
            
        return InsertBlockAtIndex(m_replacement_pointer, core_id);

        m_replacement_pointer = (m_replacement_pointer + 1) % m_associativity;
    }

    LOG_PRINT_ERROR("Error finding replacement index");
}

void
CacheSetDBASP::updateReplacementIndex(UInt32 accessed_index)
{
    UInt8 recencyPosition = 20; //20 is a invalid recency postion in 16 way cache.
    
    //printf("\nUpdation: SetID=%u\n", m_setID);

    if (accessed_index >= m_associativity)
    {
        LOG_PRINT_ERROR("\nUpdateReplacmentError: AccessedIndex=%u", accessed_index);
    }
    
    /* If block access count have reached saturation limit MAX_BLOCK_COUNT,
     * keep the counter saturated.
     */
    if (MAX_BLOCK_COUNT != m_block_access[accessed_index])
    {
        m_block_access[accessed_index]++;
    }
    
    /* On hit, find which recency position was re-referenced */
    recencyPosition = m_rrip_bits[accessed_index];
    //printf("\nUpdation at Index:%u RecencyIndex:%u\n", accessed_index, recencyPosition);
    
    /* Increment the recency counter corresponding to the recency location */
    if (0 == m_block_owner[accessed_index])
    {
        g_recencyCounterC0[recencyPosition]++;
        g_numTotalBlocksHitC0++;
    }
    else if (1 == m_block_owner[accessed_index])
    {
        g_recencyCounterC1[recencyPosition]++;
        g_numTotalBlocksHitC1++;
    }
    else
    {
        LOG_PRINT_ERROR("Error updating Recency Counters");
    }
   
    #if 0
    printf("[Newton]: Recency Position:%u Recency Counts C0:%lu, C1:%lu\n",
           recencyPosition, g_recencyCounterC0[recencyPosition],
           g_recencyCounterC1[recencyPosition]);
    #endif       

    /* As per SRRIP paper, SRRIP-HP performs better than SRRIP-FP, hence
     * setting the RRPV values directly to 0 is more beneficial than
     * decreasing it slowly.
     */ 
    insertBlockToMRUposition(accessed_index, m_rrip_bits, m_associativity);
    m_rrip_bits[accessed_index] = 0;
}

