  
                                                                       
                                                                     
   

#ifndef DMA_H
#define DMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  struct dma_result
  {
    uint32_t pid;
    uint64_t source;
    uint64_t destination;
    uint64_t length;
    uint32_t bytes_transferred;
    unsigned int interrupts;
    uint64_t cpu_instructions;
    int status;
  };

                                                                             
                                                                          
  int dma_start (void);
  void dma_shutdown (void);

                                                                      
                                                                         
                                                                              
                                          
  int dma_submit (uint32_t pid, uint64_t source, uint64_t destination,
                  uint64_t length);
  int dma_submit_reserved (uint32_t pid, uint64_t source,
                           uint64_t destination, uint64_t length);
  int dma_pid_busy (uint32_t pid);
  int dma_is_busy (void);
  void dma_record_cpu_instruction (void);

                                                                              
                                                                             
  int dma_take_completion (struct dma_result *result);

                                                                   
  void dma_done_callback (void);

#ifdef __cplusplus
}
#endif

#endif
