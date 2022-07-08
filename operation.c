#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

#include "def_helper.h"
#include "operation.h"
#include "xxHash/xxhash.h"


#ifdef USZRAM_ZAPI
#include "compressors/uszram-zapi.h"
#else
#include "compressors/uszram-zapi2.h"
#endif

#ifdef USZRAM_PTH_RW
#  include "locks/uszram-pth-rw.h"
#elif defined USZRAM_PTH_MTX
#  include "locks/uszram-pth-mtx.h"
#else
#  include "locks/uszram-std-mtx.h"
#endif

//Global Variable Declaration

static char initialized;
static struct page pgtbl[PAGE_COUNT];
static struct lock lktbl[LOCK_COUNT];
static struct worker_argu worker_argus[WORKER_NUM];
static pthread_t worker_threads[WORKER_NUM];
static stat stats;
static struct lock stat_lock;



//Static Helper Functions

static uint64_t hash(uint_least64_t key)
{
	XXH64_hash_t hash;

	hash = XXH64(&key, sizeof(uint_least64_t), 0);
	if(hash == 0)
	{
		printf("hash == 0\n");
	}


	#if OPERATION_DEBUG
	printf("OPERATION_DEBUG_DEBUG: The hash value is %lu\n",hash);

	#endif

	return hash;
}



static int maybe_reallocate(struct page *pg, size_t old_size,
			    size_t new_size)
{
	if (old_size == new_size)
		return 0;
	if (new_size == 0) {
		free(pg->compr_data);
		pg->compr_data = NULL;
	} else if (new_size < old_size) {
		pg->compr_data = realloc(pg->compr_data, new_size);
	} else {
		free(pg->compr_data);
		pg->compr_data = malloc(new_size);
	}
	return new_size - old_size;
}


static char* change_checked(char* new_blk, char* old_blk, int blk_size,int* cg_nums, unsigned char* cg_stats_out) {

	short total_nums = blk_size/BYTE_PER_CG;  //256 8  32

	short change_nums = 0;

	char * blk_cg = calloc(1, BYTE_PER_CG);

	for(int i=0;i<total_nums;i++)
	{
		if(memcmp(new_blk+i*BYTE_PER_CG, old_blk+i*BYTE_PER_CG, BYTE_PER_CG)!=0)
		{

			int byte_index = i/8;    // 32/8 4 bytes 32bit  0000000000000000000000000000000000000
			int bit_offset = i%8;

			cg_stats_out[byte_index] |= 1<<bit_offset;

			change_nums +=1;

			if(change_nums >1)
			{

				char * ret = realloc(blk_cg, BYTE_PER_CG*change_nums);

				if(ret == NULL)
				{
					fprintf(stderr, "Realloc failed \n");
					return NULL;
				}

				blk_cg = ret;


			}


			memcpy(blk_cg+(change_nums-1)*BYTE_PER_CG, new_blk+i*BYTE_PER_CG,BYTE_PER_CG);
			
		}
	}

	*cg_nums = change_nums;

	
	#if DEBUG_MODE
		printf("DEBUG MODE:In change checked cg_nums:%d change_nums %d\n",*cg_nums, change_nums);
	#endif


	if(change_nums ==0)
	{
		free(blk_cg);
		return NULL;
	}
	else
		return blk_cg;

}

static void change_unpacked(char* orginal,unsigned char status[SEG_PER_BLK*2],  char* change_buffer, int blk_size) {
	short total_nums = blk_size/BYTE_PER_CG;  //256(512) 8  32(64)
	short change_nums = 0;

	for(int i=0;i<total_nums;i++){
		int byte_index = i/8;    // 32/8 4 bytes 32bit  0000000000000000000000000000000000000
		int bit_offset = i%8;

		if((status[byte_index] & (1U << bit_offset)) !=0)
		{
			#if DEBUG_MODE
			printf("DEBUG MODE:In change unpacked i: %d bit byte_index: %d bit_offset %d\n", i, byte_index, bit_offset);
			#endif

			memcpy(orginal+i*BYTE_PER_CG, change_buffer+change_nums*BYTE_PER_CG, BYTE_PER_CG);
			change_nums +=1;
		}
	}

	#if DEBUG_MODE
		printf("DEBUG MODE:In change unpacked change_nums %d\n", change_nums);
	#endif


}

static int uszram_decompress_page_no_cg(struct page * pg, char* pg_buffer){
	int ret = decompress(pg, ONE_PAGE_SIZE, pg_buffer);

	return ret;
}

static int uszram_compress_page(struct page * pg, char * pg_buffer){
	char compr_pg[ONE_PAGE_SIZE];

	int new_size = compress(pg_buffer, compr_pg); // The new size returns the new compressed size

	int ret = maybe_reallocate(pg, pg->compr_size, new_size);

	memcpy(pg->compr_data, compr_pg, new_size);

	//printf("The diff value is %d\n", new_size - pg->compr_size);
	//printf("The new size is %d, pg->compr_size is %u\n", new_size, pg->compr_size);

	pg->t_size += (new_size - pg->compr_size);
	pg->compr_size = new_size;
	

	#if DEBUG_MODE
		printf("DEBUG MODE: The new compress page size is %d\n", new_size);
	#endif 

	return ret;

}


//ret = uszram_apply_cg_to_page(pg, pg_buffer);  
//apply all cg_buffer to the entire page
static void apply_cg_to_page(struct page * pg, char* data) {
	char * addr;
	int blk_size;

	cg_buffer * cp = pg->cg_buffer_head;

	#if DEBUG_MODE
		printf("Applying change buffer to the whole page in page flush\n");
	#endif


	while(cp) {
		//printf("Applying change buffer to block %u  decompression!\n", cp->blk_id);
		//printf("Applying change buffer to block %u  decompression!\n", cp->blk_id);
		#if DEBUG_MODE
		printf("Applying change buffer to block %u  decompression!\n", cp->blk_id);
		#endif

		if(cp ->blk_id ==0)
		{
			addr = data;
			blk_size = HOT_BLOCK_SIZE;
		}
		else
		{
			addr =  data + HOT_BLOCK_SIZE+BLOCK_SIZE *(cp->blk_id-1);
			blk_size = BLOCK_SIZE;
		}
		
		change_unpacked(addr, cp->status, (char*)cp->data, blk_size);
		cp = cp->next;
	}
}
  
//apply cg_buffer to only one blk
static void apply_cg_to_tar_blk(struct page * pg, char* data, uint_least8_t blk_id) {
	char * addr;
	int blk_size;

	cg_buffer * cp = pg->cg_buffer_head;

	while(cp !=NULL) {
		
		if(cp ->blk_id ==blk_id) // Find the cg_buffer for tar_blk
		{

			#if DEBUG_MODE
				printf("Applying change buffer to block %u  decompression!\n", cp->blk_id);
			#endif

			if(blk_id ==0)
			{
				addr = data + HOT_BLOCK_SIZE *cp->blk_id;
				blk_size = HOT_BLOCK_SIZE;
			}else
			{
				addr = data + HOT_BLOCK_SIZE+BLOCK_SIZE *(cp->blk_id-1);
				blk_size = BLOCK_SIZE;
			}

			change_unpacked(addr, cp->status, (char*)cp + sizeof(cg_buffer), blk_size);

			return;
		}
		cp = cp->next;

	}
	
	#if DEBUG_MODE
		printf("Do not find the cg_buffer for blk %u\n", blk_id);
	#endif

}


static void update_hot_blk(struct page * pg, page_init* data){

	hot_blk * new_hot_blk = &data->new_hot_blk;
	new_hot_blk->kv_nums = 0;


	int hot_start = pg->hot_front;
	int hot_end = pg->hot_tail;

	if(hot_end < hot_start) //Make sure hot_end larger than hot_start 
	{
		hot_end +=HOT_ITEM_NUM*4;
	}

	char new_hot_items[HOT_ITEM_NUM];
    int new_fill_num = 0;

	
	for(int i =hot_end;i>hot_start; i--)
	{
		if(new_fill_num ==HOT_ITEM_NUM)
			break;

		if(new_fill_num == 0)
		{
			int hot_index = i%(HOT_ITEM_NUM*4);

			new_hot_items[new_fill_num] = pg->hot_items[hot_index];
			new_fill_num +=1;


			unsigned blk_index = 0;
			unsigned blk_loc = 0;


			blk_index |= pg->hot_items[hot_index] >>4;   // The most significant 4 bits means the blk index
			blk_loc   |= pg->hot_items[hot_index] & 0x0f;  // The least significant 4 bits means the blk location



			#if DEBUG_MODE
			printf("DEBUG MODE: In update_hot_blk, blk index:%u blk_loc:%u (first one)\n", blk_index, blk_loc);
			#endif
			

            blk * tar_data_blk = &data->new_blks[blk_index-1]; //Find the tar_blk to copy kv_item data
	

			kv_item * temp_hot_kv = &(new_hot_blk->items[0]);
            kv_item * temp_data_kv = &(tar_data_blk->items[blk_loc]);


			temp_hot_kv->key = temp_data_kv->key;
			memcpy(temp_hot_kv->value, temp_data_kv->value, VALUE_SIZE);

			new_hot_blk->kv_nums +=1;

			new_hot_items[0] = pg->hot_items[hot_index];


		}else
		{
			int already_exist = 0;

			int hot_index = i%(HOT_ITEM_NUM*4);

			for(int j = 0; j< new_fill_num; j++)
			{
				if(pg->hot_items[hot_index]== new_hot_items[j])
				{
					already_exist =1;
					break;
				}
			}

			if(already_exist ==1)
				continue;
			else
			{
				new_hot_items[new_fill_num] = pg->hot_items[hot_index];
	

				unsigned blk_index = 0;
				unsigned blk_loc = 0;


				blk_index |= pg->hot_items[hot_index] >>4;   // The most significant 4 bits means the blk index
				blk_loc   |= pg->hot_items[hot_index] & 0x0f;  // The least significant 4 bits means the blk location

				#if DEBUG_MODE
				printf("DEBUG MODE: In update_hot_blk, blk index:%u blk_loc:%u \n", blk_index, blk_loc);
				#endif


				blk * tar_data_blk = &data->new_blks[blk_index-1]; //Find the tar_blk to copy kv_item data

				kv_item * temp_hot_kv = &(new_hot_blk->items[new_fill_num]);
				kv_item * temp_data_kv = &(tar_data_blk->items[blk_loc]);


				temp_hot_kv->key = temp_data_kv->key;
				memcpy(temp_hot_kv->value, temp_data_kv->value, VALUE_SIZE);

				new_hot_blk->kv_nums +=1;
				new_fill_num +=1;

				#if DEBUG_MODE
				printf("DEBUG MODE: In update_hot_blk, blk index:%u blk_loc:%u key is %lu \n", blk_index, blk_loc, temp_hot_kv->key );
				#endif



			}
		}
	}
    
	memcpy(pg->old_hot_items, new_hot_items, HOT_ITEM_NUM);
	//memset(pg->hot_items, 0, HOT_ITEM_NUM*4);
	//pg->hot_front = 0;
	//pg->hot_rear = 0;
}


// return 0 not exist, 1 exist not hot, 2 exist hot, 3 in overflow buffer
static int do_kv_update(op_item* op_data, page_init * pg_buffer, struct page * pg){

	#if DEBUG_MODE
		printf("DEBUG MODE: In do_kv_update, the start index for the target blk is %u and blk id is %d\n",HOT_BLOCK_SIZE+ BLOCK_SIZE* (op_data->blk_id-1),op_data->blk_id );
	#endif
	//printf("DEBUG MODE: In do_kv_update, the blk id is %u\n",op_data->blk_id );
	blk * tar_blk = &(pg_buffer->new_blks[(op_data->blk_id)-1]);

    int tar_kv_nums = tar_blk-> kv_nums;

	hot_blk * tar_hot_blk = &pg_buffer->new_hot_blk;

	int hot_kv_nums = tar_hot_blk-> kv_nums;
	int hot_exist = 0;

	#if DEBUG_MODE
		printf("DEBUG MODE: In do_kv_update, hot_kv_nums: %d\n", hot_kv_nums);
	#endif


	for(int i=0;i<hot_kv_nums;i++)  
	{
		if(tar_hot_blk->items[i].key == op_data->key) // Find the key in the hot_blk and update the value
		{
			//printf("DEBUG MODE: In do_kv_update, find the key in hot block\n");
			#if DEBUG_MODE
				printf("DEBUG MODE: In do_kv_update, find the key in hot block\n");
			#endif

			memcpy(tar_hot_blk->items[i].value, op_data->value, VALUE_SIZE);
			hot_exist = 1;
		}
	}
	//int temp_kv_nums = 0;
	
	int exist = 0;

	for(int i=0;i<tar_kv_nums;i++)
	{
		if(tar_blk->items[i].key == op_data->key) // Find the key in the tar_blk and update the value
		{
			memcpy(tar_blk->items[i].value, op_data->value, VALUE_SIZE);
			if(hot_exist == 1)
				exist = 2;
			else 
			{
				//printf("DEBUG MODE: In do_kv_update, find the key in the block(not hot)\n");
				//sleep(5);
				//printf("THe key is %lu\n", op_data->key);
				#if DEBUG_MODE
				printf("DEBUG MODE: In do_kv_update, find the key in the block(not hot)\n");
				#endif
				exist = 1;
			}
				
		}
	}

	if(exist == 0) // Try to search the overflow buffer
	{
		of_item* temp = pg->of_buffer_head;

		while(temp!=NULL)
		{
			if(temp->key == op_data->key)
			{
				memcpy(temp->value, op_data->value, VALUE_SIZE);
				#if DEBUG_MODE
				printf("DEBUG MODE: In do_kv_update, find the key in the overflow lists\n");
				#endif
				exist = 3;
			}
			temp = temp->next;
		}


	}

	return exist;
}

// return 0 success, 1 failed
// This function will append the new kv_item to the blk
static int do_kv_insert(op_item* op_data, page_init * pg_buffer){

	#if DEBUG_MODE
			printf("DEBUG MODE: In do_kv_insert, the start index for the target blk is %u and blk id is %d\n",HOT_BLOCK_SIZE+ BLOCK_SIZE* (op_data->blk_id-1),op_data->blk_id );
	#endif

	blk * tar_blk = &(pg_buffer->new_blks[op_data->blk_id-1]);

    int tar_kv_nums = tar_blk-> kv_nums;


    #if DEBUG_MODE
		printf("DEBUG MODE: The blk kv_nums is %d\n", tar_kv_nums);
		
	#endif


	if(tar_kv_nums == ITEM_PER_BLK) // The Blk does not have enough space for the new kv_item
	{

		#if DEBUG_MODE
			printf("DEBUG MODE: In do_kv_insert, key overflow ocurr\n");
			//sleep(1);
		#endif


		return 1;
	}
	else
	{	//printf("The tar_kv_num is %d\n", tar_kv_nums);
		tar_blk->items[tar_kv_nums].key = op_data->key;
		memcpy(tar_blk->items[tar_kv_nums].value, op_data->value, VALUE_SIZE);
		tar_blk->kv_nums +=1;
		return 0;
	}

}


//Not Finished
static int do_kv_delete(op_item* op_data, page_init * pg_buffer){
	blk * tar_blk = &(pg_buffer->new_blks[op_data->blk_id-1]);

    int tar_kv_nums = tar_blk-> kv_nums;
	//int temp_kv_nums = 0;
	
	int exist = 0;

	for(int i=0;i<tar_kv_nums;i++)
	{
		if(tar_blk->items[i].key == op_data->key) // Find the key in the blk and delete the value
		{
			//memcpy(tar_blk->items[i].value, op_data->value, VALUE_SIZE);
			exist = 1;
		}
	}

	return exist;
}



/*Flush the data of op_buffer lists to the page
  this function will be called by client and 
  background worker function
*/

static int uszram_flush_pg(uint_least64_t pg_addr){
	uint_least64_t lk_addr = pg_addr /PG_PER_LOCK;
    struct lock * lk = lktbl + lk_addr;

    lock_as_writer(lk);

    struct page * pg = pgtbl + pg_addr;


    page_init pg_buffer;
    page_init org_pg_buffer;

    int ret;


    if(pg->compr_data == NULL) // THe first time write the page
    {

    	#if DEBUG_MODE
				printf("DEBUG MODE: The page data is been written the first time\n");
		#endif

		pg_buffer.new_hot_blk.kv_nums = 0;

		for(int i=0;i<BLK_PER_PG;i++)
			pg_buffer.new_blks[i].kv_nums = 0;



		int remain_op_counts = pg->op_counts;

		#if DEBUG_MODE
				printf("DEBUG MODE: The page op counts is %u\n",remain_op_counts);
		#endif

		//APPLY the OP_buffer LIST to pg_buffer(where the pg_buffer data is )
		
		op_buffer* temp = pg->op_buffer_head;
		while(temp!=NULL)                                     //flush op_buffer links data to pg_buffer
		{
			for(int i =0; i<temp->item_nums; i++)
			{
				if(temp->items[i].op ==3)
				{
					int ret = do_kv_insert(&(temp->items[i]), &pg_buffer); // ret = 0 succ, ret = 1 failed
					if(ret == 0)
					{
						remain_op_counts -=1;
						temp->items[i].op =2;

					}else                                                 //Overflow ocurr
					{
						of_item* temp_of = malloc(sizeof(of_item));
						temp_of->key = temp->items[i].key;
						memcpy(temp_of->value, temp->items[i].value, VALUE_SIZE);
						temp_of->blk_id = temp->items[i].blk_id;
						temp_of->next = pg->of_buffer_head;
						pg->of_buffer_head = temp_of;
						pg->of_counts +=1;

						pg->t_size +=sizeof(of_item);

						#if DEBUG_MODE
						printf("DEBUG MODE: Add the kv_item with key %lu into the overflow lists\n", temp->items[i].key);
						#endif

					}

				}else if(temp->items[i].op ==1){
					do_kv_delete(&(temp->items[i]), &pg_buffer);

				}
			}   
			temp = temp->next;

		}

	    //Recompress the page
	    //First move the new hot items to the hot block

	    //update_hot_blk(pg,&pg_buffer);
	    uszram_compress_page(pg, (char *)&pg_buffer);
   
		//free old op buffer
		op_buffer * temp1 = pg->op_buffer_head;
		while(temp1!=NULL)
		{
			op_buffer * temp2 = temp1;
			temp1 = temp1->next;	
			free(temp2);
			//printf("Come HERE\n");
			pg->t_size -=sizeof(op_buffer);
		}


		pg->op_buffer_head = NULL;
		pg->op_counts = 0;
		unlock_as_writer(lk);

		return 0;


	}


    ret = uszram_decompress_page_no_cg(pg, (char *)&pg_buffer);                      //Decompress the entire page without apply cg_buffer link data 

    memcpy((char *)&org_pg_buffer, (char *)&pg_buffer, ONE_PAGE_SIZE);                     //Keep the page data without cg_buffer


    if(ret < 0){
    	fprintf(stderr, "Decompress Page Failed when flush page\n");
    	unlock_as_writer(lk);
    	return ret;
    }

    apply_cg_to_page(pg, (char *)&pg_buffer);                            //apply the cg_buffer lists to the entire page 


    int new_put_ops =0, tol_succ_ops = 0;
    int update_ops = 0;
	int remain_op_counts = pg->op_counts;
	int delete_ops = 0;

	uint_least64_t cg_blks = 0;
	int hot_blk_cg = 0;


    op_buffer* temp = pg->op_buffer_head;
    //op_buffer* temp_prev = pg->op_buffer_head;

    while(temp!=NULL)                                     //flush op_buffer links data to pg_buffer
    {
		for(int i =0; i<temp->item_nums; i++)
		{
			if(temp->items[i].op ==3)
			{

				#if DEBUG_MODE
				printf("DEBUG MODE: Try to update item with key %lu in the blk\n",temp->items[i].key);
				#endif

				//printf("THe item blk_id is %u\n", temp->items[i].blk_id);
				int ret = do_kv_update(&(temp->items[i]), &pg_buffer, pg); //only write the new data to the hot_blk && block when the key is already exist

				if(ret ==0) // new kv op_item op is keep in op_buffer wait to be recompress
				{
					new_put_ops +=1;
					tol_succ_ops +=1;

				}else if(ret ==1) // old kv, set op_item op to be 2 which means it needs to be delete
				{
					//printf("Come here 1\n");
					update_ops +=1;
					temp->items[i].op = 2;
					tol_succ_ops +=1;
					remain_op_counts -=1;

				}else if(ret ==2) // old kv && in hot buffer, set op_item op to be 2 which means it needs to be delete
				{   
					//printf("Come here 2\n");
					update_ops +=1;
					temp->items[i].op = 2;     
					hot_blk_cg = 1;
					tol_succ_ops +=1;
					remain_op_counts -=1;
				}else if(ret == 3) // old kv in the overflow buffer linked list
				{
					update_ops +=1;
					temp->items[i].op = 4;
					tol_succ_ops +=1;
					remain_op_counts -=1;
				}
			
			}else{
				do_kv_delete(&(temp->items[i]), &pg_buffer);
				tol_succ_ops +=1;
				delete_ops =1;

			}
		
			if(temp->items[i].op !=2){ // new kv or delete kv cg_buffer do not record the update
				continue;
			}else{      // old kv update cg_buffer need to record it
				//printf("Come Here\n");
				//printf("THe op is %u\n", temp->items[i].op);
				//sleep(1);
				cg_blks |= 1<<temp->items[i].blk_id;  // Record the blk_ids needed to be update (in bit level) for example blk 1 => cg_blks |= 1<<1;

			}
			
    	}   

    	temp = temp->next;
	}
 	
 	//Update the cg_buffer linked lists
 	//Create the new linked list to replace the entire old linked list
    cg_buffer * new_cg_buffer_head =NULL;
    int new_cg_counts = 0;
  

    for(int i=0;i<BLK_PER_PG+1;i++){

		if(i == 0 &&  hot_blk_cg ==1) // Create the cg buffer for hot blk
		{
			//printf("Come herer1\n");

			#if DEBUG_MODE
				printf("DEBUG MODE: Create cg for hot block\n");
			#endif

			int cg_seg_num;

			hot_blk* new_blk = &pg_buffer.new_hot_blk;      // The hot_block with change applied
			hot_blk* old_blk = &org_pg_buffer.new_hot_blk;  // The hot_block with old data


			//unsigned char  blk_cg_stats[SEG_PER_BLK*2];

			//memset((char *)&blk_cg_stats, 0 , SEG_PER_BLK*2);


			unsigned char *  blk_cg_stats = calloc(SEG_PER_BLK*2, sizeof(unsigned char));

			char* blk_cg = change_checked((char *)new_blk, (char *)old_blk, HOT_BLOCK_SIZE, &cg_seg_num, blk_cg_stats); //Create the char buffer to record change


			cg_buffer * cg_temp = malloc(sizeof(cg_buffer)+ BYTE_PER_CG*cg_seg_num); //sizeof.. 

			pg->t_size +=(sizeof(cg_buffer)+ BYTE_PER_CG*cg_seg_num);

			memcpy(cg_temp->status, blk_cg_stats, SEG_PER_BLK*2);
			cg_temp->blk_id = 0;
			cg_temp->next = NULL;

			new_cg_counts +=cg_seg_num;

			#if DEBUG_MODE
				printf("DEBUG MODE: cg_seg_num is %d for block %d\n", cg_seg_num, i);
			#endif

			memcpy(cg_temp->data, cg_temp, BYTE_PER_CG*cg_seg_num);      //Copy the cg record to the cg_buffer data
			new_cg_buffer_head = cg_temp;
			free(blk_cg_stats);
			free(blk_cg); 

		}else if(cg_blks & (1<<i) ) // Create the cg buffer for normal blk ?
		{
			//printf("Come Here2\n");

			int cg_seg_num;
			//char new_blk[BLOCK_SIZE], old_blk[BLOCK_SIZE];
			
			blk* new_blk = &pg_buffer.new_blks[i-1];      // The block with change applied
			blk* old_blk = &org_pg_buffer.new_blks[i-1];  // The block with old data

			unsigned char *  blk_cg_stats = calloc(SEG_PER_BLK*2, sizeof(unsigned char));

			char* blk_cg = change_checked((char *)new_blk, (char *)old_blk, BLOCK_SIZE, &cg_seg_num, blk_cg_stats); //Create the char buffer to record change

			cg_buffer * cg_temp = malloc(sizeof(cg_buffer)+ BYTE_PER_CG*cg_seg_num);

			pg->t_size +=(sizeof(cg_buffer)+ BYTE_PER_CG*cg_seg_num);

			memcpy(cg_temp->status, blk_cg_stats, SEG_PER_BLK*2);



			cg_temp->blk_id = i;
			cg_temp->next = NULL;
			


			cg_buffer * temp =  new_cg_buffer_head;

			if(temp == NULL)
			{
				new_cg_buffer_head = cg_temp;
			}
			else
			{
				while(temp->next !=NULL)
				{
					temp = temp->next;
				}

				temp->next = cg_temp;

			}

			new_cg_counts +=cg_seg_num;
			//printf("DEBUG MODE: cg_seg_num is %d for block %d\n", cg_seg_num, i);

			#if DEBUG_MODE
				printf("DEBUG MODE: cg_seg_num is %d for block %d\n", cg_seg_num, i);
			#endif
			memcpy(cg_temp->data, blk_cg, BYTE_PER_CG*cg_seg_num);      //Copy the cg record to the cg_buffer data

			//printf("DEBUG MODE: THE cg_temp->data is %s\n", cg_temp->data);
			free(blk_cg_stats);
			free(blk_cg);
		}

    }

    //free older cg_buffer linked list
    //free_cg_lists(pg);
	while(pg->cg_buffer_head !=NULL)
	{
		cg_buffer* temp = pg->cg_buffer_head->next;
		free(pg->cg_buffer_head);
		pg->cg_buffer_head = temp;
		pg->t_size -=(sizeof(cg_buffer));

	}  

	pg->t_size -=BYTE_PER_CG*pg->cg_counts;
	pg->cg_buffer_head = new_cg_buffer_head;
    pg->cg_counts = new_cg_counts;

	#if DEBUG_MODE
    printf("DEBUG MODE: new cg_counts is %d\n", pg->cg_counts);
    #endif
 
    //Determine if needed recompression, multiple cases
    /*
	Case 1: cg_counts > CG_TRESHOLD

	Case 2: update_op < tol_op /2
	*/
	//int of_changed = 0;

	if((pg->cg_counts > CG_NUM_MAX) || update_ops <= tol_succ_ops/2)
	{

		#if DEBUG_MODE
				printf("DEBUG MODE: Recompression is needed for this page\n");
		#endif

		//APPLY the OP_buffer LIST to pg_buffer(where the pg_buffer data is )

		if(delete_ops == 1) //op_buffer contains several delete operation, which means there may be empty slot for the overflow buffer
		{


			of_item * temp1 = pg->of_buffer_head;
			of_item * new_of_head = NULL;


			while(temp1!=NULL)                                     //flush of_buffer links data to pg_buffer
			{
				
				op_item temp_kv;

				temp_kv.key = temp1->key;
				memcpy(temp_kv.value, temp1->value, VALUE_SIZE);
				temp_kv.blk_id = temp1->blk_id;


				int ret = do_kv_insert(&temp_kv, &pg_buffer); // ret = 0 succ, ret = 1 failed
				if(ret == 1)
				{
					of_item* temp_of = malloc(sizeof(of_item));
					temp_of->key = temp1->key;
					memcpy(temp_of->value, temp1->value, VALUE_SIZE);
					temp_of->blk_id = temp1->blk_id;
					pg->t_size +=sizeof(of_item);
					temp_of->next = new_of_head;
					new_of_head = temp_of;
					pg->of_counts +=1;
					#if DEBUG_MODE
					printf("DEBUG MODE: Add the kv_item with key %lu into the overflow lists\n", temp1->key);
					#endif


				}

				temp1 = temp1->next;   
			}


			if(new_of_head!=NULL) // New of_linked_list is created
			{
				temp1 = pg->of_buffer_head;

				while(temp1!=NULL)
				{
					of_item * temp2 = temp1;
					temp1 = temp1->next;	
					free(temp2);
					//printf("Come here \n");
					pg->t_size -=sizeof(of_item);
					pg->of_counts -=1;
				}

				pg->of_buffer_head = new_of_head;

			}

		}

		
		op_buffer* temp = pg->op_buffer_head;
		while(temp!=NULL)                                     //flush op_buffer links data to pg_buffer
		{
			for(int i =0; i<temp->item_nums; i++)
			{
				if(temp->items[i].op ==3)
				{
					int ret = do_kv_insert(&(temp->items[i]), &pg_buffer); // ret = 0 succ, ret = 1 failed
					if(ret == 0)
					{
						temp->items[i].op = 2;
						remain_op_counts -=1;
					}else                                                  // Add the kv_pair to overflow buffer
					{
						of_item* temp_of = malloc(sizeof(of_item));
						temp_of->key = temp->items[i].key;
						memcpy(temp_of->value, temp->items[i].value, VALUE_SIZE);
						temp_of->blk_id = temp->items[i].blk_id;
						temp_of->next = pg->of_buffer_head;
						pg->of_buffer_head = temp_of;
						pg->of_counts +=1;
						pg->t_size +=sizeof(of_item);

						#if DEBUG_MODE
						printf("DEBUG MODE: Add the kv_item with key %lu into the overflow lists\n", temp->items[i].key);
						#endif

					}

				}else if(temp->items[i].op ==1){
					do_kv_delete(&(temp->items[i]), &pg_buffer);

				}
			}

			temp = temp->next;   
		}


	    //Recompress the page
	    //First move the new hot items to the hot block

	    update_hot_blk(pg,&pg_buffer);
	    uszram_compress_page(pg, (char *)&pg_buffer);


	    //free cg_buffer linked list
	    //free_cg_lists(pg);
		while(pg->cg_buffer_head !=NULL)
		{
			cg_buffer* temp = pg->cg_buffer_head->next;
			free(pg->cg_buffer_head);
			pg->cg_buffer_head = temp;
			pg->t_size -=(sizeof(cg_buffer));

		}  

		pg->t_size -=BYTE_PER_CG*pg->cg_counts;


		pg->cg_counts = 0;
		pg->cg_buffer_head = NULL;


	} 
	

	//free old op buffer

	op_buffer * temp1 = pg->op_buffer_head;

	while(temp1!=NULL)
	{
		op_buffer * temp2 = temp1;
		temp1 = temp1->next;	
		free(temp2);
		pg->t_size -=sizeof(op_buffer);
	}


	pg->op_buffer_head =NULL;
	pg->op_counts = 0;

	if(pg->op_buffer_head ==NULL)
	{
		#if DEBUG_MODE
		printf("DEBUG MODE: old buffer is clean !\n");
		#endif 
	}

	#if DEBUG_MODE
	printf("DEBUG MODE: The overflow counts is %u\n", pg->of_counts);

	of_item* of_temp = pg->of_buffer_head;

	while(of_temp !=NULL)
	{
		printf("The of_item key is %lu\n",of_temp->key);
		of_temp = of_temp->next;
	}

	#endif 

	unlock_as_writer(lk);

    return 0;

}


//Worker will continuous check if the page needs recompression
static void * worker_init(void * argu)
{

	struct worker_argu * worker = (struct worker_argu *)argu;

	#if DEBUG_MODE
	printf("DEBUG_MODE: worker %d Initialization\n", worker->id);
	printf("The range start is %ld, range_count is %ld\n", worker->range_start,worker->range_count);
	#endif

	int first_time = 1;

	while(worker->status !=-1)
	{

		usleep(1000);

		if(first_time)	//Initial the flush trigger value
		{
			worker->recompr_pages =0;
			worker->op_buffer_trigger = OP_NUM_MAX;
			first_time = 0;
		}

		//Adaptively adjust the trigger threshold for page flush
		if(worker->recompr_pages <(worker->range_count/10)) // The recompr_pages are too few in each loop
		{
			if(worker->op_buffer_trigger >1)
			{
				worker->op_buffer_trigger /= 2;
			}
		}else if(worker->recompr_pages > (worker->range_count/2)) //The recompr_pages are too many in each loop
		{
			if(worker->op_buffer_trigger <OP_NUM_MAX)
			{
				worker->op_buffer_trigger *= 2;
			}
		}

		worker->recompr_pages = 0;
       
       //Loop through the corresponding table range assigned
		for(uint_least64_t i=worker->range_start;i<worker->range_start+worker->range_count;i++)
		{
			

			uint_least64_t lk_addr = i/PG_PER_LOCK;
			struct lock * lk = lktbl +lk_addr;
			lock_as_reader(lk);


			struct page * pg = pgtbl + i;

			if(pg->op_counts >=worker->op_buffer_trigger ) //Page flush is needed
			{
				unlock_as_reader(lk);
				#if DEBUG_MODE
				printf("Worker Flush trigger, the pg->op_counts is %d and the op_buffer_trigger is %d\n", pg->op_counts, worker->op_buffer_trigger );
				#endif
				//printf("i is %ld, lk_addr is %lu\n",i, lk_addr);
				//printf("Worker Flush trigger, the pg->op_counts is %d and the op_buffer_trigger is %d\n", pg->op_counts, worker->op_buffer_trigger );
				//sleep(1);
				uszram_flush_pg(i);
				worker->recompr_pages += 1;


			}else
			{
				unlock_as_reader(lk);
			}


		}
			
	}

	return NULL;

}




//*****************************************************************************************/
//Operation Functions
//*****************************************************************************************/


int uszram_init(void)
{
	if (initialized)
		return -1;
	initialize_lock(&stat_lock);

	for (uint_least64_t i = 0; i != LOCK_COUNT; ++i)         // Initial lktbl
		initialize_lock(lktbl + i);

	for (uint_least64_t i = 0; i != PAGE_COUNT; ++i)  // Initial pgtbl
	{
	
		(pgtbl+i)->compr_data = NULL;
	    (pgtbl+i)->op_buffer_head = NULL;
		(pgtbl+i)->cg_buffer_head = NULL;
		(pgtbl+i)->of_buffer_head = NULL;


		(pgtbl+i)->op_counts = 0;
		(pgtbl+i)->cg_counts = 0;
		(pgtbl+i)->of_counts = 0;


		(pgtbl+i)->compr_size = 0;
		(pgtbl+i)->t_size = sizeof(struct page);
    	//(pgtbl+i)->hot_items = {0};
    	(pgtbl+i)->hot_front = 0;
		(pgtbl+i)->hot_tail = 0;
    	//(pgtbl+i)->old_hot_items = {0};

	}

	for(int i=0; i<WORKER_NUM; i++)                        // Initial background worker
	{
		worker_argus[i].id =i;
		worker_argus[i].range_start = i* PAGE_COUNT/WORKER_NUM;
		worker_argus[i].range_count = PAGE_COUNT/WORKER_NUM;
		worker_argus[i].status = 0;
		pthread_create(&worker_threads[i],NULL, worker_init, &worker_argus[i]);
	}
    
	//Initial states
	stats.pg_num = PAGE_COUNT;
	stats.worker_num = WORKER_NUM;

	stats.item_num = 0;
	stats.item_in_op = 0;
	
	stats.get_hit = 0;
	stats.get_miss = 0;
	stats.put_hit = 0;
    stats.put_miss = 0;
    stats.delete_hit = 0;
    stats.delete_miss = 0;


	//stats.tol_compr_size = 0;
	//stats.tol_op_size = 0;
	//stats.tol_cg_size = 0;

	initialized = 1;


	return 0;
}





int uszram_exit(void)
{
	if (!initialized)
		return -1;
	initialized = 0;

	for (uint_least64_t i = 0; i != LOCK_COUNT; ++i)
		destroy_lock(lktbl + i);
	/*
	for (uint_least64_t i = 0; i != PAGE_COUNT; ++i)
		delete_pg(pgtbl + i);
	*/
	//stats.num_compr = stats.failed_compr = 0;
	for(int i=0; i<WORKER_NUM; i++){
		worker_argus[i].status = 1;
		pthread_join(worker_threads[i], NULL);
	}
	return 0;
}


int uszram_kv_put(kv_item item){
	
	uint_least64_t blk_addr = hash(item.key);

	blk_addr = blk_addr%BLOCK_COUNT;

	//Force to be in the first page;
	//blk_addr = blk_addr%14;

	uint_least64_t pg_addr = blk_addr/BLK_PER_PG;         //Determine the pg_address;
	uint_least64_t lk_addr = pg_addr /PG_PER_LOCK;
	
	uint_least64_t blk_index = (blk_addr-pg_addr*BLK_PER_PG)+1;          //The tar_blk for this item

	struct lock * lk = lktbl + lk_addr;
	lock_as_writer(lk);

    struct page * pg = pgtbl + pg_addr;
    //struct lock * lk = lktbl + lk_addr;

    #if DEBUG_MODE
    printf("THe LOCK address is %lu and page address is %lu\n", lk_addr, pg_addr);

   	printf("THe blk index is %lu  and key is %lu\n", blk_index,item.key);

   	printf("THe blk addr is %lu\n", blk_addr);
   	#endif
   
   	/*
   	if(blk_index > BLK_PER_PG)
   	{
   		printf("HERE\n");
   		printf("THE value is %ld\n", blk_index);
   		sleep(5);
   	}
	*/

	op_buffer* temp = pg->op_buffer_head;

	//The pg does not have an op_buffer list currently
	if(temp == NULL)
	{
		op_buffer* new_op = malloc(sizeof(op_buffer));
		#if DEBUG_MODE
		printf("Create the first op_buffer\n");
		#endif

		new_op->next = pg->op_buffer_head;
		new_op->item_nums = 1;
		new_op->items[0].op = 3;
		new_op->items[0].key = item.key;
		new_op->items[0].blk_id = blk_index;
		new_op->next = NULL;
		memcpy(new_op->items[0].value, item.value, VALUE_SIZE);

		pg->op_buffer_head = new_op;
		pg->op_counts +=1;

		pg->t_size +=sizeof(op_buffer);
	}
	else
	{
		int exist = 0;
		while(temp!=NULL)  
		{
			if(exist == 1)
				break;

			#if DEBUG_MODE
				printf("DEBUG MODE: The op_nums in current buffer is %u\n",temp->item_nums);
			#endif


			for(int i =0; i<ITEM_PER_OP; i++)
			{
				if(temp->items[i].key == item.key)  // The key already exist in the op buffer
				{
					temp->items[i].op =3;
					memcpy(temp->items[i].value, item.value, VALUE_SIZE);
					exist = 1;
					#if DEBUG_MODE
						printf("DEBUG MODE: The same key is found \n");
					#endif

					break;
				}

				// The key does not exist in the op buffer but current buffer have empty slot
				if(temp->next == NULL && temp->item_nums < ITEM_PER_OP && i ==  temp->item_nums)  
				{
					#if DEBUG_MODE
					printf("DEBUG MODE: Put the item in slot %d in the last op buffer\n", i);
					#endif

					temp->items[i].op =3;
					temp->items[i].key = item.key;
					temp->items[i].blk_id = blk_index;
					memcpy(temp->items[i].value, item.value, VALUE_SIZE);
					temp->item_nums +=1;
					pg->op_counts +=1;
					exist = 1;
					break;

				}
			}
			// THe key does not exist in the op buffer and current buffer does not have empty slot
			// Append a new op_buffer struct at the end of the op_buffer lists
			if(temp->next == NULL && exist ==0)
			{
				#if DEBUG_MODE
				printf("DEBUG MODE: Put the item in the first slot in a new create op buffer\n");
				#endif

				op_buffer* new_op = malloc(sizeof(op_buffer));
				new_op->next = NULL;
				new_op->item_nums = 1;
				new_op->items[0].op = 3;
				new_op->items[0].key = item.key;
				new_op->items[0].blk_id = blk_index;
				memcpy(new_op->items[0].value, item.value, VALUE_SIZE);

				temp->next = new_op;
				pg->op_counts +=1;
				exist = 1;

				pg->t_size +=sizeof(op_buffer);


			}
			temp = temp->next;
		}

	}

    if(pg->op_counts <OP_NUM_MAX)
    {

		#if DEBUG_MODE
			printf("DEBUG MODE: op_num less than threshold, no flush needed\n");
		#endif
    	unlock_as_writer(lk);

		lock_as_writer(&stat_lock);

		stats.put_hit +=1;
		stats.item_num +=1;
		stats.item_in_op +=1;


		unlock_as_writer(&stat_lock);

    	return 0;
    }else
    {

		#if DEBUG_MODE
			printf("DEBUG MODE: op_num reach threshold, flush needed\n");
		#endif

    	unlock_as_writer(lk);

		lock_as_writer(&stat_lock);

		stats.put_hit +=1;
		stats.item_num +=1;
		stats.item_in_op +=1;

		unlock_as_writer(&stat_lock);


    	int ret = uszram_flush_pg(pg_addr);
    	return ret;
    }

}


kv_item * uszram_kv_get(uint_least64_t key){
	
	uint_least64_t blk_addr = hash(key);

	blk_addr = blk_addr%BLOCK_COUNT;

	#if DEBUG_MODE

	//blk_addr = blk_addr % 14; // ONly for DEBUG

	#endif

	uint_least32_t pg_addr = blk_addr/BLK_PER_PG;         //Determine the pg_address;
	uint_least32_t lk_addr = pg_addr /PG_PER_LOCK;
	uint_least8_t blk_index = blk_addr-pg_addr*BLK_PER_PG+1;


	//Force to be in the first page;
	blk_addr = blk_addr%14;


    struct page * pg = pgtbl + pg_addr;
    struct lock * lk = lktbl + lk_addr;

   
    lock_as_reader(lk);

    #if DEBUG_MODE
		printf("DEBUG MODE: Try to get the kv_item with key %lu\n", key);
	#endif 




	//Check if the key is in the op buffer list
	op_buffer* temp = pg->op_buffer_head;
	while(temp!=NULL)
	{
		for(int i =0; i<temp->item_nums;i++)
		{
			if(key == temp->items[i].key)
			{
				#if DEBUG_MODE
					printf("DEBUG MODE: Find the item in the op buffer \n");
				#endif 

				kv_item * tar_item = malloc(sizeof(kv_item));
				tar_item->key = key;
				memcpy(tar_item->value, temp->items[i].value, VALUE_SIZE);

				unlock_as_reader(lk);



				lock_as_writer(&stat_lock);

				stats.get_hit +=1;


				unlock_as_writer(&stat_lock);


				return tar_item;

			}
		}

		temp = temp->next;

	}
	

	#if DEBUG_MODE
		printf("DEBUG MODE: Not Find the item in the op buffer!\n");
	#endif 

	//Try to find in the hot blk

	page_init pg_buffer;

	int ret = decompress(pg, HOT_BLOCK_SIZE, (char *)&pg_buffer); // Decompress till the hot blk

	#if DEBUG_MODE
		printf("DEBUG MODE: THE decompress size is %d\n",ret);
	#endif


	apply_cg_to_tar_blk(pg, (char *)&pg_buffer, 0);             // Apply cg_buffer to the hot blk

	hot_blk * tar_hot_blk = &pg_buffer.new_hot_blk;

	#if DEBUG_MODE
		printf("DEBUG MODE: The hot blk kv_nums is %u\n", tar_hot_blk->kv_nums);
	#endif


	for(int i=0;i<tar_hot_blk->kv_nums;i++)  
	{

		#if DEBUG_MODE
		printf("The key for hot %d is %lu\n", i, tar_hot_blk->items[i].key);
		#endif

		if(tar_hot_blk->items[i].key == key) // Find the key in the hot_blk and update the value
		{

			#if DEBUG_MODE
				printf("DEBUG MODE: Find the key in hot block\n");
			#endif

			kv_item * tar_item = malloc(sizeof(kv_item));


			tar_item->key = key;
			memcpy(tar_item->value, tar_hot_blk->items[i].value, VALUE_SIZE);

			//Update the hot_items record

			uint_least8_t cur_hot_front = pg->hot_front;
			uint_least8_t cur_hot_tail = pg->hot_tail;

			#if DEBUG_MODE
				printf("DEBUG MODE: Before modify the hot start and end are %u & %u!\n", cur_hot_front, cur_hot_tail);
			#endif 


			if(cur_hot_tail<cur_hot_front)
				cur_hot_tail +=HOT_ITEM_NUM*4;
			
			if(cur_hot_tail -cur_hot_front == HOT_ITEM_NUM*4 -1) // All slots are full
			{
				pg->hot_items[cur_hot_front] = pg->old_hot_items[i];

				cur_hot_front +=1;
				cur_hot_tail +=1;

				cur_hot_front = cur_hot_front%(HOT_ITEM_NUM*4);
				cur_hot_tail = cur_hot_tail%(HOT_ITEM_NUM*4);


			}else // Not all slot are full, only increment hot_tail
			{
				cur_hot_tail +=1;
				cur_hot_tail = cur_hot_tail%(HOT_ITEM_NUM*4);

				pg->hot_items[cur_hot_tail] = key;
			}

			#if DEBUG_MODE
				printf("DEBUG MODE: Before modify the hot start and end are %u & %u!\n", cur_hot_front, cur_hot_tail);
			#endif 
			
			pg->hot_front = cur_hot_front;
			pg->hot_tail = cur_hot_tail;


			unlock_as_reader(lk);




			lock_as_writer(&stat_lock);

			stats.get_hit +=1;


			unlock_as_writer(&stat_lock);


			return tar_item;
		}
	}

	
	#if DEBUG_MODE
		printf("DEBUG MODE: Not Find the item in the hot block!\n");
	#endif 

	//Try to find in the tar blk   
	int bytes_needs = HOT_BLOCK_SIZE +blk_index*BLOCK_SIZE;

	ret = decompress(pg, bytes_needs, (char *)&pg_buffer); // Decompress till the hot blk

	apply_cg_to_tar_blk(pg, (char *)&pg_buffer, blk_index);             // Apply cg_buffer to the hot blk

	blk * tar_blk = &pg_buffer.new_blks[blk_index-1];


	for(int i=0;i<tar_blk->kv_nums;i++)  
	{
		if(tar_blk->items[i].key == key) // Find the key in the blk and update the value
		{

			#if DEBUG_MODE
				printf("DEBUG MODE: Find the key in block %d\n",blk_index);
			#endif

			kv_item * tar_item = malloc(sizeof(kv_item));


			tar_item->key = key;
			memcpy(tar_item->value, tar_blk->items[i].value, VALUE_SIZE);

			//Update the hot_items record

			uint_least8_t cur_hot_front = pg->hot_front;
			uint_least8_t cur_hot_tail = pg->hot_tail;

			#if DEBUG_MODE
				printf("DEBUG MODE: Before modify the hot start and end are %u & %u!\n", cur_hot_front, cur_hot_tail);
			#endif 


			if(cur_hot_tail<cur_hot_front)
				cur_hot_tail +=HOT_ITEM_NUM*4;
			
			if(cur_hot_tail -cur_hot_front == HOT_ITEM_NUM*4 -1) // All slots are full
			{
				
				uint_least8_t hot_value = 0x00;

				hot_value |= (blk_index &0x0f)<<4;
				hot_value |= (i &0x0f);


				pg->hot_items[cur_hot_front] = hot_value;


				cur_hot_front +=1;
				cur_hot_tail +=1;

				cur_hot_front = cur_hot_front%(HOT_ITEM_NUM*4);
				cur_hot_tail = cur_hot_tail%(HOT_ITEM_NUM*4);


			}else // Not all slot are full, only increment hot_tail
			{
				cur_hot_tail +=1;
				cur_hot_tail = cur_hot_tail%(HOT_ITEM_NUM*4);


				uint_least8_t hot_value = 0x00;

				hot_value |= (blk_index &0x0f)<<4;
				hot_value |= (i &0x0f);


				pg->hot_items[cur_hot_tail] = hot_value;
			}

			#if DEBUG_MODE
				printf("DEBUG MODE: Before modify the hot start and end are %u & %u and hot_value is %u!\n", cur_hot_front, cur_hot_tail,pg->hot_items[cur_hot_tail] );
			#endif 
			
			pg->hot_front = cur_hot_front;
			pg->hot_tail = cur_hot_tail;

			unlock_as_reader(lk);



			lock_as_writer(&stat_lock);

			stats.get_hit +=1;


			unlock_as_writer(&stat_lock);



			return tar_item;
		}
	}

	//Find in the overflow buffer
	of_item* temp_of = pg->of_buffer_head; 

	while(temp_of!=NULL)
	{
		if(temp_of->key == key)
		{
			#if DEBUG_MODE
				printf("DEBUG MODE: Find the key in overflow lists\n");
			#endif

			kv_item * tar_item = malloc(sizeof(kv_item));

			tar_item->key = key;
			memcpy(tar_item->value, temp_of->value, VALUE_SIZE);


			unlock_as_reader(lk);

			lock_as_writer(&stat_lock);

			stats.get_hit +=1;


			unlock_as_writer(&stat_lock);



			return tar_item;
		}

		temp_of = temp_of->next;

	}




#if DEBUG_MODE
	printf("DEBUG MODE: Not Find the item in the at all!\n");
#endif 
	unlock_as_reader(lk);



	lock_as_writer(&stat_lock);

	stats.get_miss +=1;

	unlock_as_writer(&stat_lock);

	return NULL;

}


void print_status(void)
{

	lock_as_reader(&stat_lock);

	printf("//////////////////////////////////////\n");
	printf("pg_num is     %lu\n", stats.pg_num);
	printf("worker_num is %lu\n", stats.worker_num);
	printf("item_num is   %lu\n", stats.item_num);
	printf("item_in_op is %lu\n", stats.item_in_op);

	printf("get_hit is    %lu\n", stats.get_hit);
	printf("get_miss is   %lu\n", stats.get_miss);

	printf("put_hit is    %lu\n", stats.put_hit);
	printf("put_miss is   %lu\n", stats.put_miss);
	printf("//////////////////////////////////////\n");

	uint64_t tol_page_size=0;

	for(int i = 0; i< PAGE_COUNT;i++)
	{
		if(i < 100)
		{
			printf("The page %d tol_size is %u pg mata is %lu\n",i, pgtbl[i].t_size,sizeof(struct page));
			printf("THE page op_counts is %u, cg_counts is %u\n",pgtbl[i].op_counts, pgtbl[i].cg_counts);
			printf("The page compr_size is %u\n", pgtbl[i].compr_size);
			printf("THe page of_counts is %u\n", pgtbl[i].of_counts);
		}
	
		tol_page_size += pgtbl[i].t_size;

	}
	printf("Total page size is %lu\n", tol_page_size);

	printf("The size of op_buffer is %lu\n", sizeof(op_buffer));
	printf("The size of cg_buffer is %lu\n", sizeof(cg_buffer));


	//printf("Size of page table is %ld\n", sizeof(pgtbl));


	unlock_as_reader(&stat_lock);

}