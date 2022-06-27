#ifndef DEF_HELPER_H
#define DEF_HELPER_H



#define DEBUG_MODE 0


#define HOT_ITEM_NUM 8
#define BLOCK_SIZE 256
#define HOT_BLOCK_SIZE 512
#define SEG_PER_BLK BLOCK_SIZE/(8*8)

#define VALUE_SIZE 8
#define ITEM_PER_OP 4


#define USZRAM_ZAPI
//#define USZRAM_ZAPI2


/*
 * - USZRAM_STD_MTX selects a plain mutex from the C standard library
 * - USZRAM_PTH_MTX selects a plain mutex from the pthread library
 * - USZRAM_PTH_RW selects a readers-writer lock from the pthread library
 */
#define PG_PER_LOCK 4
#define USZRAM_PTH_MTX
//#define USZRAM_STD_MTX


//Define the page and blk size

#define PAGE_COUNT 10000
#define BLK_PER_PG 14
#define BLOCK_COUNT (PAGE_COUNT * BLK_PER_PG)


//#define BLOCK_SIZE    256
#define ONE_PAGE_SIZE     4096
#define PG_PER_LOCK 4
#define LOCK_COUNT    ((PAGE_COUNT - 1) / PG_PER_LOCK + 1)

//Define the size of SUBPG for ZAPI2 especially
#define SUBPG_SIZE 1024

//Define the number of background workers
#define WORKER_NUM 4
#define WORKER_SLEEP 0

//Define the threshold for op_counts
#define OP_NUM_MAX 8

//Define the threshhold for cg_counts
#define CG_NUM_MAX 32  //256 bytes
#define SEG_PER_BLK BLOCK_SIZE/(8*8)
#define BYTE_PER_CG 8

//Define the hot kv items for each page
#define HOT_ITEM_NUM 8




typedef struct kv_item_{
	uint_least64_t key;
	char value[VALUE_SIZE];
}kv_item;


#define ITEM_PER_BLK (BLOCK_SIZE-sizeof(uint_least8_t))/sizeof(struct kv_item_)
#define BLK_PEDDING (BLOCK_SIZE - sizeof(uint_least8_t) -ITEM_PER_BLK* sizeof(struct kv_item_))


typedef struct /*__attribute__((packed))*/ _blk
{
	uint_least8_t kv_nums;
	char blk_ped[BLK_PEDDING]; 
	kv_item items[ITEM_PER_BLK];
}	blk;

#define ITEM_PER_HOT_BLK (HOT_BLOCK_SIZE-sizeof(uint_least8_t))/sizeof(struct kv_item_)
#define HOT_BLK_PEDDING (HOT_BLOCK_SIZE - sizeof(uint_least8_t) -ITEM_PER_HOT_BLK* sizeof(struct kv_item_))


typedef struct /*__attribute__((packed))*/ hot_blk_
{
    uint_least8_t kv_nums;
	char blk_ped[HOT_BLK_PEDDING]; 
	kv_item items[ITEM_PER_HOT_BLK];
}	hot_blk;



typedef struct page_init_
{
	hot_blk new_hot_blk;
	blk new_blks[BLK_PER_PG];

}page_init;


/*
op = 0  put  op = 1 delete op = 2 op_item invalid
*/
typedef struct op_item_{
	uint_least8_t op;
	uint_least8_t blk_id;
	uint_least64_t key;
	char value[VALUE_SIZE];
}op_item;

typedef struct op_buffer_{
	struct op_buffer_* next;
	uint_least8_t item_nums;
	op_item items[ITEM_PER_OP];
}op_buffer;

typedef struct /*__attribute__((packed))*/ cg_buffer_{
	unsigned char status[SEG_PER_BLK*2];
	uint_least8_t blk_id;
	struct cg_buffer_* next;
	char data[];
}cg_buffer;



struct page {
	char * compr_data;
	op_buffer * op_buffer_head;
	cg_buffer * cg_buffer_head;

	uint_least8_t op_counts;
	uint_least8_t cg_counts;
	uint_least16_t compr_size;
	//uint_least16_t t_size;
    uint_least8_t hot_items[HOT_ITEM_NUM*4];
    uint_least8_t hot_front;
    uint_least8_t hot_tail;
    uint_least8_t old_hot_items[HOT_ITEM_NUM];

};


struct worker_argu
{
	uint32_t id;
	uint64_t range_start;
	uint64_t range_count;
	int32_t status;

	uint64_t recompr_pages;
	uint32_t op_buffer_trigger;

};


typedef struct stat_
{

	uint64_t pg_num;
	uint64_t worker_num;
	uint64_t cur_storesize;

	uint64_t item_num;
	uint64_t item_in_op;

	uint64_t get_hit;
	uint64_t get_miss;

	uint64_t put_hit;
	uint64_t put_miss;

	uint64_t set_hit;
	uint64_t set_miss;

	uint64_t delete_hit;
	uint64_t delete_miss;



}stat;




#endif 
