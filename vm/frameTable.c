#include "vm/frameTable.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "kernel/list.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "vm/pageTable.h"
#include "lib/kernel/bitmap.h"
#include "threads/vaddr.h"
#include "threads/pte.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

//-------------------------VARIABLES LOCALES--------------------------------//
//Lock utilizado en operaciones de insert a listas, entre otros usos.
struct lock ftLock;
struct lock ftLock2;
//-------------------------VARIABLES LOCALES--------------------------------//

//Se incializan elementos necesarios para la frame table
void init_frameTable(){
  //Se incializan los locks
  lock_init(&ftLock);//Para todos los procesos menos eviction
  lock_init(&ftLock2);//Para el proceso de eviction
  //Se incializa la frame table list
  list_init (&frame_table);
}

//Se obtiene la pagina y se guarda en la lista frame_table
void * get_pageFT(enum palloc_flags pFlags){
  //Allocate memory for the frame table entry
  struct frame_table_entry *FTE = malloc(sizeof(struct frame_table_entry));
  void * fa = NULL; 
  //Se obtiene la virtual address de una free page para una user page
   if (pFlags & PAL_USER)
    {
      if (pFlags & PAL_ZERO)
        fa = palloc_get_page (PAL_USER | PAL_ZERO);
      else
        fa = palloc_get_page (PAL_USER);
    }


  if(fa != NULL){
    lock_acquire(&ftLock);
    //Se guardan la información en la frame table entry
    FTE->frame = fa;
    FTE->thread_owner = thread_current();
    list_push_back(&frame_table, &FTE->frame_elem);
    lock_release(&ftLock);
  }
  else
    fa = evict_page();

  return fa; 
}
void* evict_page(){
  lock_acquire(&ftLock2);
  struct frame_table_entry *victima;
  //Se selecciona la victima para saber que frame sera evicted
  victima = select_victim();
  //Se guarda la victima en swap
  bool save_success = evicted_save(victima);

  if(victima == NULL)
    PANIC("El algoritmo no encontró una victima");
  //No se pudo guardar la victima en swap
  if(!save_success)
    PANIC("Error al guardar el evicted frame");

  //Se limpia la victima
  victima->thread_owner = thread_current();
  victima->user_vaddr = NULL;
  victima->pte = NULL;
  lock_release(&ftLock2);
  return victima->frame;
}

struct frame_table_entry* select_victim(){
  lock_acquire(&ftLock2);
  struct frame_table_entry* felem;
  struct list_elem *element;
  struct frame_table_entry* victim = NULL;
  bool accessed = false;
  //Se itera en la frame table para encontrar la siguiente victima
  for(element = list_begin(&frame_table);element != list_end(&frame_table);
      element = list_next(element)){
      felem = list_entry(element, struct frame_table_entry, frame_elem);
      /*Se busca una página que no haya sido accesada, de esta forma forma es más fácil
        hacer seleccionar la victima*/
      if(!pagedir_is_accessed(felem->thread_owner->pagedir,felem->user_vaddr)){
        victim = felem;
        /*Se hace remove y se vuelve a insertar en la lista con el fin de mantener a los 
        frames viejos hasta el head de la lista*/
        list_remove(element);
        list_push_back(&frame_table, element); 
        break;
      } 
      else//De forma contraria, se limpian los bits
        pagedir_set_accessed(felem->thread_owner->pagedir,felem->user_vaddr, accessed);
  }
  /*Si no hubo victima porque ninguna tenía el accessed bit en 0,
    entonces la siguiente victima es el frame más antiguo*/
  if(!victim){
    element = list_begin(&frame_table);
    victim = list_entry(element, struct frame_table_entry, frame_elem);
  }
  lock_release(&ftLock2);
  return victim;
}

void 
getPTEandUvaddr(void* frame, void* upage, uint32_t* pte){
  //Se verifica que no se tenga vacía la frame table
  ASSERT(!list_empty(&frame_table));

  lock_acquire (&ftLock);
  struct frame_table_entry* FTE;
  struct list_elem *element;
    //Se busca si el frame tiene una frame table asociada
    for(element = list_begin(&frame_table);element != list_end(&frame_table);
      element = list_next(element)){
      FTE = list_entry(element, struct frame_table_entry, frame_elem);
      if(FTE->frame == frame)
        break; //Se encontró el frame en la fram table
      else 
        FTE = NULL; //Si termina la iteración sin un break entonces null
    }

  if(FTE){//Sí se encontró una frame table entry
    //Como se encontró se guardan los nuevos campos en la frame table entry
    FTE->user_vaddr = upage; //User virtual address mapeada al frame
    FTE->pte = pte;//pte 
  } 
  else
    PANIC("No se encontró una frame table entry");

  lock_release (&ftLock);
}

bool 
evicted_save(struct frame_table_entry* victima){

  struct thread* owner = victima->thread_owner;
  //Se busca la supplemental page table asociada al thread owner
  struct sup_page_table_entry* vSPTE = get_SPTE(victima->user_vaddr, &owner->SPT);
  //Si no se encuentra una spte asociada se crea una
  if(!vSPTE){
    vSPTE = malloc(sizeof(struct sup_page_table_entry));
    vSPTE->user_vaddr = victima->user_vaddr;
    vSPTE->status = IN_SWAP;
    bool success = storeInSPT(vSPTE, &owner->SPT);
    if(!success)
      return false;
  }
  size_t index;
  //PEDIENTE IMPLEMENTACIONES DE MMAP
  bool page_dirty = pagedir_is_dirty(owner->pagedir, vSPTE->user_vaddr);
  //Nos aseguramos que el status sea 0 o IN_SWAP
  if(page_dirty || (vSPTE->status != FROM_EXE)){
    index = write_from_swap(vSPTE->user_vaddr);
    if(index == BITMAP_ERROR)
      return false;
    else
      //Se setea el status como IN_SWAP
      vSPTE->status = IN_SWAP|vSPTE->status;
  }
  
  //Se llena de ceros el frame de la victima
  memset(victima->frame, 0, PGSIZE);
  //Se guarda el indice el cual se le asignó en el swap bitmap
  vSPTE->swap_index = index;
  //Como ya no está en memoria y ahora esta en swap, se pone false
  vSPTE->page_loaded = false;
  //Verdadero si la página es writable. 
  bool is_writable = *(victima->pte) & PTE_W; //PTE_W -> 1 sí es writable, 0 read-only
  vSPTE->is_swap_W = is_writable;
  //Se invalida el PTE asociado al proceso
  pagedir_clear_page (owner->pagedir, vSPTE->user_vaddr);

  return true;
}

//Remueve el frame de la lista de frames
void free_frameTable(void *frame){
  //Se verifica que no se tenga vacía la frame table
  ASSERT(!list_empty(&frame_table));

  lock_acquire (&ftLock);
  
  struct frame_table_entry *felem;
  struct list_elem *element;

  //Se itera en la frame table para encontrar y remover de el frame de la frame table
  for(element = list_begin(&frame_table);element != list_end(&frame_table);
  	  element = list_next(element)){
      felem = list_entry(element, struct frame_table_entry, frame_elem);
      if(felem->frame == frame){
        list_remove(element);  
        //Se libera el lock
        lock_release (&ftLock);
        return;
      }
  }
  //Se libera la página 
  palloc_free_page (frame);
  //Se libera el lock
  lock_release (&ftLock);
}