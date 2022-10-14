char* change_checked(char* new_blk, char* old_blk, int blk_size,int* cg_nums, char* cg_stats_out) {
  
	short total_nums = blk_size/BYTE_PER_CG;

	short change_nums = 0;

	char * blk_cg = calloc(1, BYTE_PER_CG);

	for(int i=0;i<total_nums;i++)
	{
		if(memcmp(new_blk+i*BYTE_PER_CG, old_blk+i*BYTE_PER_CG, BYTE_PER_CG)!=0)
		{

			int byte_index = i/8;
			int bit_offset = i%8;

			cg_stats_out[byte_index] |= 1<<bit_offset;

			change_nums +=1;

			if(change_nums >1)
				realloc(blk_cg, BYTE_PER_CG*change_nums);


			memcpy(new_blk+(change_nums-1)*BYTE_PER_CG, new_blk+i*BYTE_PER_CG,BYTE_PER_CG);

		}
	}

	*cg_nums = change_nums;


	if(change_nums ==0)
		return NULL;
	else
		return blk_cg;

}