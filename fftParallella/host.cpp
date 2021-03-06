#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libgen.h>
#include <time.h>

#include <unistd.h>
#include <assert.h>

#include <e-hal.h>

#include "include/tiff_handler.h"

#define BILLION 1E9

enum UserInterrupt{
        NotDone = 0x0,
        Done    = 0x1,
        Error   = 0x2,
		Run     = 0x3,
		Idle    = 0x4
};

int main(int argc, char* argv[]){

	float *img, *imgComplex;
	int sizeX, sizeY;
	char *s;	

	if(argc != 4){
		printf("usage: executable <name of input data> <imageWidth> <imageLength>");
		exit(1);
	}else{
		s = argv[1];
		sizeX = atoi(argv[2]);
		sizeY = atoi(argv[3]);
	}

	img = (float*)calloc(sizeX*sizeY, sizeof(float));
	readTIFF(s, img);
	printf("%f \n", img[25]);	

	//Transform image to complex form
	imgComplex = (float*)calloc(2*512*sizeX, sizeof(float));
	for(int i = 0; i < sizeX*sizeY; i++){
		imgComplex[2*i] = img[i];
	}

	//TODO: mit device verbinden + verarbeitung
	char *hostExecutable = strdup(argv[0]);
	char *epiphanyExecutable = (char*)malloc(sizeof(char) * (strlen(hostExecutable) + strlen(E_EXECUTABLE) + 1 + 1));
	sprintf(epiphanyExecutable, "%s/%s", dirname(hostExecutable), E_EXECUTABLE);

	//initialize epiphany device
	e_platform_t platform;
	e_epiphany_t dev;

	e_init(nullptr);
	e_reset_system();
	e_get_platform_info(&platform);
	
	//start workgroup with all e-cores
	if(e_open(&dev, 0, 0, platform.rows, platform.cols) != E_OK){
		printf("e_open failed\n");
		exit(1);
	}
 
	//load epiphany program into workgroup, but don't start it yet
	if(e_load_group(epiphanyExecutable, &dev, 0, 0, platform.rows, platform.cols, E_FALSE) == E_ERR){
		printf("epiphany program could not be loaded into workgroup\n");
	 	exit(1);
	}

  	//copy data
	int nn = 256;
	int inverse = 1;
	
	/*float *dataComplex = (float*)calloc((2*nn+1), sizeof(float));
	for(int i = 0; i < nn; i++){
		dataComplex[2*i] = i;	
	}*/

	UserInterrupt init = UserInterrupt::NotDone;
	for(unsigned int x = 0; x < platform.cols; x++){
		for(unsigned int y = 0; y < platform.rows; y++){
			e_write(&dev, y, x, 0x40, &nn, sizeof(nn));
            e_write(&dev, y, x, 0x44, &inverse, sizeof(inverse));
        }
    }
	e_start_group(&dev);
	
	//Zeitmessung
	struct timespec start, end, startwrite, endwrite, startread, endread, startcalc, endcalc;
	clock_gettime(CLOCK_REALTIME, &start);
	double timewrite = 0, timeread = 0, timecalc = 0;
	
	for(int line = 0; line < 32; line++){	
		init = UserInterrupt::Run;
		clock_gettime(CLOCK_REALTIME, &startwrite);
		for(unsigned int x = 0; x < platform.cols; x++){
			for(unsigned int y = 0; y < platform.rows; y++){				
				e_write(&dev, y, x, 0x6000, imgComplex+(x+y*platform.cols)*2*nn+line*2*nn*16, sizeof(float)*(2*nn));
			}
		}
		for(unsigned int x = 0; x < platform.cols; x++){
			for(unsigned int y = 0; y < platform.rows; y++){	
				e_write(&dev, y, x, 0x24, &init, sizeof(init));
			}
		}
		clock_gettime(CLOCK_REALTIME, &endwrite);
		clock_gettime(CLOCK_REALTIME, &startcalc);
		timewrite += (endwrite.tv_sec - startwrite.tv_sec) + (endwrite.tv_nsec - startwrite.tv_nsec)/BILLION;
		
        //e_reset_group(&dev);
		/*init = UserInterrupt::Run;
        for(unsigned int x = 0; x < platform.cols; x++){
			for(unsigned int y = 0; y < platform.rows; y++){
				
			}
		}*/
		//start device program
		//e_start_group(&dev);
		
		//wait until the epiphany is done
		UserInterrupt epiphanyDone = UserInterrupt::NotDone;
		while(epiphanyDone != UserInterrupt::Done){
			e_read(&dev, 0, 0, 0x24, &epiphanyDone, sizeof(epiphanyDone));
			if(epiphanyDone == UserInterrupt::NotDone) 
				usleep(1000); 
		}
		clock_gettime(CLOCK_REALTIME, &endcalc);
		timecalc += (endcalc.tv_sec - startcalc.tv_sec) + (endcalc.tv_nsec - startcalc.tv_nsec)/BILLION;
		//printf("1 %d\n", epiphanyDone);
		assert(epiphanyDone == UserInterrupt::Done);
		
		clock_gettime(CLOCK_REALTIME, &startread);
		for(unsigned int x = 0; x < platform.cols; x++){
	      		for(unsigned int y = 0; y < platform.rows; y++){
				e_read(&dev, y, x, 0x6000, imgComplex+(x+y*platform.cols)*2*nn+line*2*nn*16, sizeof(float)*(2*nn));
			}
		}
		clock_gettime(CLOCK_REALTIME, &endread);
		timeread += (endread.tv_sec - startread.tv_sec) + (endread.tv_nsec - startread.tv_nsec)/BILLION;
	}
	
	//time measurement
	clock_gettime(CLOCK_REALTIME, &end);	
	
	double time = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec)/BILLION;
	
	printf("Zeit Schreibvorgang: %lf s\n", timewrite);
	printf("Zeit Lesevorgang: %lf s\n", timeread);
	printf("Zeit Rechnen: %lf s\n", timecalc);
	printf("Dauer Berechnung auf Epiphany: %lf s\n", time);
	
	uint32_t cycles, fpops;
	e_read(&dev, 0, 0, 0x48, &cycles, sizeof(uint32_t));
	e_read(&dev, 0, 0, 0x4C, &fpops, sizeof(uint32_t));
	
	printf("FFT:\n");
	for(int i = 0; i < sizeX*sizeY; i++){
		//printf("X[%d] = (%.2f + j %.2f)\n", i, imgComplex[2*i], imgComplex[2*i+1]);
		img[i] = imgComplex[2*i];
	}

	printf("%d cycles, %d fpops\n", cycles, fpops);
	
	writeTiff((char*)"out.tif", img, sizeY, sizeX);

	free(img);
	e_reset_system();
	return 0;
}
