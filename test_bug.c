#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "def_helper.h"


#define VALUE_SIZE 4



int main(){

	kv_item item1;

	blk blk1;

	printf("Size of item is %ld \n", sizeof(kv_item));

	printf("Size of blk1 is %ld  item is %ld pedding is %ld \n", sizeof(blk), ITEM_PER_BLK, BLK_PEDDING);


	printf("Size of hot_blk1 is %ld item nums is %ld pedding is %ld\n",sizeof(hot_blk), ITEM_PER_HOT_BLK, HOT_BLK_PEDDING);

	printf("Size of cg_buffer is %ld\n", sizeof(cg_buffer));

	short cg_blks = 0;

	cg_blks |= 1<<3;
	cg_blks |= 1<<2;


	printf("Size of page init is %ld\n",sizeof(page_init));

	printf("The cg_blk is %d \n", cg_blks);

	char blk_index = 4;   // The most significant 4 bits means the blk index
	char blk_loc = 5;  // The least significant 4 bits means the blk location

	char hot_value = 0;

	hot_value |= (blk_loc &0x0f);

	hot_value |= (blk_index &0x0f)<<4;


	printf("The value of hot value is %u\n", hot_value);



	int blk_index1 = hot_value >>4;

	int blk_loc1 = hot_value &0x0f; 


	printf("The value of index1 is %u and the value of loc1 is %u\n", blk_index1, blk_loc1);



	op_buffer * new_op_head = calloc(1, sizeof(op_buffer));

	new_op_head->item_nums = 0;
	new_op_head->next = NULL;



	op_buffer * new_temp = new_op_head;

	while(new_temp !=NULL) // Go to the last op buffer
	{
		printf("come here\n");
		sleep(1);
		if(new_temp->next == NULL)
		{
			break;
			printf("Here Here\n");
			
		}

		new_temp = new_temp->next;

	}


}