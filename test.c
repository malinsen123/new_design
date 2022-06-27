#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <time.h>



#include "def_helper.h"
#include "operation.h"
int main(){

	kv_item item1;

	int ret = uszram_init();

	struct timespec start1, end1;
	double total_put_time, duration1, total_get_time;

	total_put_time = 0.0;
	total_get_time = 0.0;


	for(int i=0; i< 10900;i++)
	{
		item1.key= i;

		memcpy(item1.value, "helloyu\n",8 );

		timespec_get(&start1, TIME_UTC);

		ret = uszram_kv_put(item1);

		timespec_get(&end1, TIME_UTC);

		duration1 = end1.tv_sec - start1.tv_sec + (end1.tv_nsec - start1.tv_nsec) / 1000000000.0;

		total_put_time += duration1;

	}

	printf("The total put time is %.3lf\n", total_put_time);

	printf("Wait 5s for get ready\n");
	sleep(5);

	
	/*
	for(int i=0; i< 10000;i++)
	{
		item1.key= i;

		memcpy(item1.value, "fdsff\n",6 );


		ret = uszram_kv_put(item1);


	}

	*/

	for(int i=0; i< 10900;i++)
	{

		//printf("Try to get item with key %d\n",i);
		//printf("/////////////////////// \n");


		timespec_get(&start1, TIME_UTC);

		kv_item * item2 = uszram_kv_get(i);


		timespec_get(&end1, TIME_UTC);

		duration1 = end1.tv_sec - start1.tv_sec + (end1.tv_nsec - start1.tv_nsec) / 1000000000.0;

		total_get_time += duration1;

		if(item2 !=NULL)
		{
			free(item2);
			//printf("The item value is %s\n", item2->value);
		}
	}


	printf("The total get time is %.3lf\n", total_get_time);


	print_status();

	
	/*

	for(int i=8; i< 16;i++)
	{
		item1.key= i;

		memcpy(item1.value, "abcde\n",6 );


		ret = uszram_kv_put(item1);


	}




	for(int i=0; i< 8;i++)
	{

		printf("Try to get item with key %d\n",i);
		printf("/////////////////////// \n");
		kv_item * item2 = uszram_kv_get(i);

		if(item2 !=NULL)
			printf("The item value is %s\n", item2->value);
	}




	*/

	//print_status();

	/*
	for(int i=9; i< 17;i++)
	{
		item1.key= i;

		memcpy(item1.value, "hello\n",6 );



		ret = uszram_kv_put(item1);


	}



	*/

}

