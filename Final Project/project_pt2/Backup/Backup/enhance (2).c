/*************************************************************************************
			       DEPARTMENT OF ELECTRICAL AND ELECTRONIC ENGINEERING
					   		     IMPERIAL COLLEGE LONDON 

 				      EE 3.19: Real Time Digital Signal Processing
					       Dr Paul Mitcheson and Daniel Harvey

				        		 PROJECT: Frame Processing

 				            ********* ENHANCE. C **********
							 Shell for speech enhancement 

  		Demonstrates overlap-add frame processing (interrupt driven) on the DSK. 

 *************************************************************************************
 				             By Danny Harvey: 21 July 2006
							 Updated for use on CCS v4 Sept 2010
 ************************************************************************************/
/*
 *	You should modify the code so that a speech enhancement project is built 
 *  on top of this template.
 */
/**************************** Pre-processor statements ******************************/
//  library required when using calloc
#include <stdlib.h>
//  Included so program can make use of DSP/BIOS configuration tool.  
#include "dsp_bios_cfg.h"

/* The file dsk6713.h must be included in every program that uses the BSL.  This 
   example also includes dsk6713_aic23.h because it uses the 
   AIC23 codec module (audio interface). */
#include "dsk6713.h"
#include "dsk6713_aic23.h"

// math library (trig functions)
#include <math.h>

/* Some functions to help with Complex algebra and FFT. */
#include "cmplx.h"      
#include "fft_functions.h"  

// Some functions to help with writing/reading the audio ports when using interrupts.
#include <helper_functions_ISR.h>

#define WINCONST 0.85185			/* 0.46/0.54 for Hamming window */
#define FSAMP 8000.0				/* sample frequency, ensure this matches Config for AIC */
#define FFTLEN 256					/* fft length = frame length 256/8000 = 32 ms*/
#define NFREQ (1+FFTLEN/2)			/* number of frequency bins from a real FFT */
#define OVERSAMP 4					/* oversampling ratio (2 or 4) */  
#define FRAMEINC (FFTLEN/OVERSAMP)	/* Frame increment */
#define CIRCBUF (FFTLEN+FRAMEINC)	/* length of I/O buffers */

#define OUTGAIN 16000.0				/* Output gain for DAC */
#define INGAIN  (1.0/16000.0)		/* Input gain for ADC  */
// PI defined here for use in your code 
#define PI 3.141592653589793
#define TFRAME FRAMEINC/FSAMP       /* time between calculation of each frame */

/******************************* Global declarations ********************************/

/* Audio port configuration settings: these values set registers in the AIC23 audio 
   interface to configure it. See TI doc SLWS106D 3-3 to 3-10 for more info. */
DSK6713_AIC23_Config Config = { \
			 /**********************************************************************/
			 /*   REGISTER	            FUNCTION			      SETTINGS         */ 
			 /**********************************************************************/\
    0x0017,  /* 0 LEFTINVOL  Left line input channel volume  0dB                   */\
    0x0017,  /* 1 RIGHTINVOL Right line input channel volume 0dB                   */\
    0x01f9,  /* 2 LEFTHPVOL  Left channel headphone volume   0dB                   */\
    0x01f9,  /* 3 RIGHTHPVOL Right channel headphone volume  0dB                   */\
    0x0011,  /* 4 ANAPATH    Analog audio path control       DAC on, Mic boost 20dB*/\
    0x0000,  /* 5 DIGPATH    Digital audio path control      All Filters off       */\
    0x0000,  /* 6 DPOWERDOWN Power down control              All Hardware on       */\
    0x0043,  /* 7 DIGIF      Digital audio interface format  16 bit                */\
    0x008d,  /* 8 SAMPLERATE Sample rate control        8 KHZ-ensure matches FSAMP */\
    0x0001   /* 9 DIGACT     Digital interface activation    On                    */\
			 /**********************************************************************/
};

// Codec handle:- a variable used to identify audio interface  
DSK6713_AIC23_CodecHandle H_Codec;

float *inbuffer, *outbuffer;   		/* Input/output circular buffers */
float *inframe, *outframe;          /* Input and output frames */
float *inwin, *outwin;              /* Input and output windows */
float ingain, outgain;				/* ADC and DAC gains */ 
float cpufrac; 						/* Fraction of CPU time used */
volatile int io_ptr=0;              /* Input/ouput pointer for circular buffers */
volatile int frame_ptr=0;           /* Frame pointer */
int i=0;
float *M1,*M2,*M3,*M4,*N,*N_i,*P;
complex *buffer;
float lpf_weight_speech;
float lpf_weight_noise;
int 	nc=1;
int 	lpf_switch=1;
int 	lpf_power_switch=1;
int 	lpf_N_switch=1;
float 	tau1=0.01;//Unit ms, range 20~80 ms, control weight of latest input, trade-off between voice and noise,0.04
float	tau2=0.08;
float 	alpha=	2;//trade-off between speech and noise
float   lambda=0.07;//trade-off between background noise and musical noise, ALLOWS LOWER in this example
float	ratio=0.3;//corresponds to SNR -5 dB
float	alpha1=6;
float	alpha2=1;
 /******************************* Function prototypes *******************************/
void init_hardware(void);    	/* Initialize codec */ 
void init_HWI(void);            /* Initialize hardware interrupts */
void ISR_AIC(void);             /* Interrupt service routine for codec */
void process_frame(void);       /* Frame processing routine */
void function_switch();
void noise_reduction();
void noise_estimate();
void frame_in();
/********************************** Main routine ************************************/
void main()
{      

  	int k; // used in various for loops
  	
/*  Initialize and zero fill arrays */  

	inbuffer	= (float *) calloc(CIRCBUF, sizeof(float));	/* Input array */
    outbuffer	= (float *) calloc(CIRCBUF, sizeof(float));	/* Output array */
	inframe		= (float *) calloc(FFTLEN, sizeof(float));	/* Array for processing*/
    outframe	= (float *) calloc(FFTLEN, sizeof(float));	/* Array for processing*/
    inwin		= (float *) calloc(FFTLEN, sizeof(float));	/* Input window */
    outwin		= (float *) calloc(FFTLEN, sizeof(float));	/* Output window */
	M1			= (float *) calloc(FFTLEN, sizeof(float));	
	M2			= (float *) calloc(FFTLEN, sizeof(float));	
	M3			= (float *) calloc(FFTLEN, sizeof(float));	
	M4			= (float *) calloc(FFTLEN, sizeof(float));	
	N_i			= (float *) calloc(FFTLEN, sizeof(float));	
	N			= (float *) calloc(FFTLEN, sizeof(float));
	P			= (float *) calloc(FFTLEN, sizeof(float));
	buffer		= (complex *) calloc(FFTLEN, sizeof(complex));	
	/* initialize board and the audio port */
  	init_hardware();
  
  	/* initialize hardware interrupts */
  	init_HWI();    
  
	/* initialize algorithm constants */  
                       
  	for (k=0;k<FFTLEN;k++)
	{                           
	inwin[k] = sqrt((1.0-WINCONST*cos(PI*(2*k+1)/FFTLEN))/OVERSAMP);
	outwin[k] = inwin[k]; 
	} 
  	ingain=INGAIN;
  	outgain=OUTGAIN;        

 							
  	/* main loop, wait for interrupt */  
  	while(1) 	process_frame();
}
    
/********************************** init_hardware() *********************************/  
void init_hardware()
{
    // Initialize the board support library, must be called first 
    DSK6713_init();
    
    // Start the AIC23 codec using the settings defined above in config 
    H_Codec = DSK6713_AIC23_openCodec(0, &Config);

	/* Function below sets the number of bits in word used by MSBSP (serial port) for 
	receives from AIC23 (audio port). We are using a 32 bit packet containing two 
	16 bit numbers hence 32BIT is set for  receive */
	MCBSP_FSETS(RCR1, RWDLEN1, 32BIT);	

	/* Configures interrupt to activate on each consecutive available 32 bits 
	from Audio port hence an interrupt is generated for each L & R sample pair */	
	MCBSP_FSETS(SPCR1, RINTM, FRM);

	/* These commands do the same thing as above but applied to data transfers to the 
	audio port */
	MCBSP_FSETS(XCR1, XWDLEN1, 32BIT);	
	MCBSP_FSETS(SPCR1, XINTM, FRM);	
	

}
/********************************** init_HWI() **************************************/ 
void init_HWI(void)
{
	IRQ_globalDisable();			// Globally disables interrupts
	IRQ_nmiEnable();				// Enables the NMI interrupt (used by the debugger)
	IRQ_map(IRQ_EVT_RINT1,4);		// Maps an event to a physical interrupt
	IRQ_enable(IRQ_EVT_RINT1);		// Enables the event
	IRQ_globalEnable();				// Globally enables interrupts

}
        
/******************************** process_frame() ***********************************/  
void process_frame(void)
{
	int k, m; 
	int io_ptr0;  
	lpf_weight_speech =exp(-TFRAME/tau1);
	lpf_weight_noise=exp(-TFRAME/tau2);
	/* work out fraction of available CPU time used by algorithm */    
	cpufrac = ((float) (io_ptr & (FRAMEINC - 1)))/FRAMEINC;  
		
	/* wait until io_ptr is at the start of the current frame */ 	
	while((io_ptr/FRAMEINC) != frame_ptr); 
	
	/* then increment the framecount (wrapping if required) */ 
	if (++frame_ptr >= (CIRCBUF/FRAMEINC)) frame_ptr=0;
 	
 	/* save a pointer to the position in the I/O buffers (inbuffer/outbuffer) where the 
 	data should be read (inbuffer) and saved (outbuffer) for the purpose of processing */
 	io_ptr0=frame_ptr * FRAMEINC;
	
	/* copy input data from inbuffer into inframe (starting from the pointer position) */ 
	 
	m=io_ptr0;
    for (k=0;k<FFTLEN;k++)
	{                           
		inframe[k] = inbuffer[m] * inwin[k]; 
		if (++m >= CIRCBUF) m=0; /* wrap if required */
	} 
	
	/************************* DO PROCESSING OF FRAME  HERE **************************/
		
	frame_in();
	noise_estimate();
	noise_reduction();
	function_switch();
	i++;
	/********************************************************************************/
	
    /* multiply outframe by output window and overlap-add into output buffer */  
                           
	m=io_ptr0;
    
    for (k=0;k<(FFTLEN-FRAMEINC);k++) 
	{    										/* this loop adds into outbuffer */                       
	  	outbuffer[m] = outbuffer[m]+outframe[k]*outwin[k];   
		if (++m >= CIRCBUF) m=0; /* wrap if required */
	}
        
    for (;k<FFTLEN;k++) 
	{                           
		outbuffer[m] = outframe[k]*outwin[k];   /* this loop over-writes outbuffer */        
	    m++;
	}	                                   
}        
/*************************** INTERRUPT SERVICE ROUTINE  *****************************/

// Map this to the appropriate interrupt in the CDB file
   
void ISR_AIC(void)
{       
	short sample;
	/* Read and write the ADC and DAC using inbuffer and outbuffer */
	
	sample = mono_read_16Bit();
	inbuffer[io_ptr] = ((float)sample)*ingain;
		/* write new output data */
	mono_write_16Bit((int)(outbuffer[io_ptr]*outgain)); 
	
	/* update io_ptr and check for buffer wraparound */    
	
	if (++io_ptr >= CIRCBUF) io_ptr=0;
}

/************************************************************************************/

//Processing when import frame
void frame_in(){
	int k;
	float mag;
	for(k=0;k<FFTLEN;k++){
		buffer[k]=cmplx(inframe[k],0);	
	}	 
	//perform FFT
	fft(FFTLEN,buffer);
	//rotate every 2.5s
	if(i== 312){
	M4=M3;
	M3=M2;
	M2=M1;	
	M1=M4;						      		
	i=0;
	}
	//Import to M1
	if(i==0){
		for(k=0;k<FFTLEN;k++){
			M1[k]=cabs(buffer[k]);			
		}	
	}
	else{
		for(k=0;k<FFTLEN;k++){
			mag=cabs(buffer[k]);
			M1[k]=(1-lpf_weight_speech)*mag+lpf_weight_speech*M1[k];
		}
	}
}
//Calculate noise buffer
void noise_estimate(){
	int i;
	for (i=0;i<FFTLEN;i++)
	{                          
		N_i[i] = M1[i];
		if(N_i[i]>M2[i]){N_i[i]=M2[i];}
		if(N_i[i]>M3[i]){N_i[i]=M3[i];}
		if(N_i[i]>M4[i]){N_i[i]=M4[i];}
		if((cabs(buffer[i])-N_i[i])<10*N_i[i]){
			alpha=alpha1;
		}
		else{
			alpha=alpha2;
		}

		N[i]=alpha*(1-lpf_weight_noise)*N_i[i]+lpf_weight_noise*N[i];
	}	
}
//reduce noise using noise buffer
void noise_reduction(){
	int k;
	float a,b,gain;
    for (k=0;k<FFTLEN;k++)
	{   
		a=1-N[k]/cabs(buffer[k]);
		b=lambda;
		gain=a;	
		if(gain<b){
			gain=b;
		}                       
		buffer[k] = rmul(gain,buffer[k]);
	} 
	ifft(FFTLEN,buffer);
}
//switch of noise cancelling function on
void function_switch(){
	int k;
	if(nc==1){
		for (k=0;k<FFTLEN;k++)
		{ 
			outframe[k] = buffer[k].r;
		} 
	}
	else{
		for (k=0;k<FFTLEN;k++)
		{ 
			outframe[k] = inframe[k];
		} 
	}
}
