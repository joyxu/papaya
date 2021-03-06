#include<disp.h>
#include<utils.h>
#include<mm.h>
#include<valType.h>
#include<proc.h>
#include<bootinfo.h>
#include<elf.h>
#include<fork.h>
#include<linux/sched.h>
struct slab_head * mm_cache;
struct slab_head * vm_area_cache;
u32 gmemsize=0;
char testbuf[1024];
void init_memory(void){
	assert(sizeof(union cr3) == 4 && sizeof( union pte) == 4 && 
		   sizeof(union linear_addr) == 4);
	heap_init();
	/**detect pphysical memory: print memory information and init global variable 'gmemsize'*/
	int mem_segnum = realmod_info->mem_segnum;
	struct mem_seginfo *memseg =realmod_info->mem_seginfo;
	oprintf("%12s%12s%10s\n","start","len","type");
	for(int i=0; i<mem_segnum; i++){
		oprintf("%12x%12x%10s\n",memseg[i].base_low,memseg[i].len_low,\
								memseg[i].type==1?"free":"occupied");
		if(memseg[i].type == 1 && memseg[i].base_low > gmemsize) \
			gmemsize = memseg[i].base_low+memseg[i].len_low;
	}
/*	oprintf("physical memory size:%x\n",gmemsize);	*/

	/**initialize mem_map*/
	int mapsize = G_PGNUM * sizeof(struct page);
	mem_map = kmalloc0(mapsize);

	size_of_zone[0] = 16*0x100000;
	if(gmemsize > ZONE_HIGHMEM_PA){
		size_of_zone[1] = (896-16)*0x100000;
		size_of_zone[2] = gmemsize - 896*0x100000;
	}
	else{
		size_of_zone[1] = gmemsize - 16*0x100000;
		size_of_zone[2] = 0;
	}
}

/*
31                4                             0
+-----+-...-+-----+-----+-----+-----+-----+-----+
|     Reserved    | I/D | RSVD| U/S | W/R |  P  |
+-----+-...-+-----+-----+-----+-----+-----+-----+

    P: When set, the fault was caused by a protection violation.
    When not set, it was caused by a non-present page.
    W/R: When set, write access caused the fault; otherwise read access.
    U/S: When set, the fault occurred in user mode; otherwise in supervisor mode.
    RSVD: When set, one or more page directory entries contain reserved bits which are set to 1.
    This only applies when the PSE or PAE flags in CR4 are set to 1.
    I/D: When set, the fault was caused by an instruction fetch.
    This only applies when the No-Execute bit is supported and enabled. 
*/

#if 0
/**
 * page-error exception handler
 * alloc a physical page and mapped it to the ill virtual page
 * 2，如果是ring0发生页错误，那打印出来的esp是无效的，因为pregs指向里根本没
 * 保存esp，同级堆栈切换。
 * 3, 空指针解引用会page_fault。现在就会。 因为0号页表项是空的。 以后也会一直留空，就是
 * 为了捕获内核的0地址访问。 内核要访问0地址，直接访问3G就行了。
 */
void do_page_fault(stack_frame *preg, unsigned err_code){
	//oprintf("pgerr trapped from ring %u,curr_process:%s\n",current->pregs->cs&3,current->p_name);
	pgerr_count++;
	u32 err_addr;
	__asm__ __volatile__(
			"movl %%cr2,%0"
			:"=r" (err_addr)
			:
	);
	if(err_addr == 0) spin("attempt to access address 0");
	//u32 err_code=current->pregs->err_code;
	//oprintf("page error: err_code:%x, err_addr:%x,eip:%x,esp:%x\n", err_code, err_addr, preg->eip, preg->esp);
		//oprintf("%s: %c %c\n",(err_code&1)?"page protection error":"page not exist error",(err_code&B(0100))?'U':'S',(err_code&B(0010))?'W':'R');
	oprintf("sick process:%s,pcb:%x\n",current->p_name,current);
	if((err_code&B(0001)) == 0){
	/**page fault:page not exist*/
		/*为init进程加载代码页*/
		if(strcmp(current->p_name, "init") == 0 && (err_addr>>12 == 0x8048)){
			int vpg = err_addr >> 12;
			map_pg((u32 *)KV(current->cr3), vpg, alloc_page(__GFP_ZERO, 0), PG_USU, PG_RWR);	/*加载代码页.text, .rodata*/
			map_pg((u32 *)KV(current->cr3), vpg+1, alloc_page(__GFP_ZERO, 0), PG_USU, PG_RWW);		/*加载数据页 .data.bss*/
			//底下这一句是有效的，可见chgpg函数没问题。但在
			//fork里就是不生效。
			cell_read("../src/usr/src/init", testbuf);
			oprintf("%s",testbuf);
			current->pregs->eip = loadelf(testbuf);
		}
		/*堆栈页是在这里加载的，是通用加载*/
		else{
			map_pg((u32*)KV(current->cr3),err_addr>>12,alloc_page(__GFP_ZERO,0),PG_USU,PG_RWW);
		}	
		if(pgerr_count==2) spin("pgerr_count == 2");
	}

	/*WP exception*/
	else if(err_code & B(0010)){
		int vpg = err_addr >> 12;
		unsigned *dir = (u32 *)(KV(current->mm->cr3.value) & ~0xfff);
		unsigned *tbl = (u32 *)(KV(dir[PG_H10(vpg)]) & ~0xfff);
		unsigned pte = tbl[PG_L10(vpg)];
		unsigned *ppte = tbl + PG_L10(vpg);
		struct page *page = pte_page(pte);
		if(page->cow_shared >= 1){
			oprintf("meet a PG_COW\n");
			
			if(page->cow_shared >= 2){	/*assign it a new clone page frame*/
				struct page	*newpage = alloc_pages(__GFP_ZERO, 0);
				char *pageframe = (char *)page_va(newpage);
				memcp(pageframe, (char *)page_va(page), 4096);

				*ppte = (pte & 0xfff) | ( (newpage-mem_map) << 12 );
			}
			if(page->cow_shared == 1){
					
			}
			*ppte |= PG_RWW;
			page->cow_shared--;
		}	
		else{
			oprintf("normal WP error, not PG_COW\n");
		}
	}
	else if((err_code & B(0010)) == 0){
		spin("read error");
	}
	else{
		spin("can not handle page protection error\n");
	} 
/*	oprintf("do pgerr_count-- and return now\n");*/
	pgerr_count--;
	return;
}

#endif

void map_pg(u32*dir,int vpg,int ppg,int us,int rw){
	/**check the validation of page dir-entry. does it point to a valid page
	 * table?if not,alloc a clean page as page-table*/
	u32 *dirent = dir + PG_H10(vpg);
	if((*dirent & PG_P) == 0){
/*		oprintf("@map_pg bad entry,alloc one page as table\n");*/
		//在pgdir的entry里，不对U/S, R/W做控制，留给page table
		*dirent  =__pa(__alloc_page(__GFP_ZERO) )| PG_USU | PG_RWW | PG_P;	
	}
	u32 *tbl = (u32*)KV((*dirent)>>12<<12);/**trip attr-bit*/
/*	oprintf("@map_pg tbl at:%x\n",tbl);*/
	tbl[PG_L10(vpg)] = ppg<<12|us|rw|PG_P;
	FLUSH_TLB;
}

/* for quick test, to be removed soon
 * febd1000, febfxxxx, ...
 * 8139网卡的寄存器群是映射在1G左右的地方的，我们把它映射到4G附近的地方来访问。
 * 它是memory mapped IO, 我们访问就用mov,当然是经过MMU的。
 */
void temp_mmio_map(void){
	/* intel i3 motherboard, rtl8139, mmio base, 0x3baff0000
	 * intel pentium motherboard, rtl8139, mmio base, 0x3febxxxxx
	 * intel 945 mmio base, 0x3dbff000
	 *  we map from 0xfba00000 ==> 0xfbb00000, 0xfeb80000 ===> 0xfec00000
	 */
	oprintf(" temp mmio map begin >>>>>>>>>\n");
	unsigned *dir = (unsigned *)0xc0100000;
	//u32 start = 0xfeb80000;
	//u32 end = 0xfec00000 - 0x1000;					/* 应该减去0x1000的，无伤大雅*/
	u32 start = 0xfba00000;
	u32 end = 0xfbb00000;
	for(unsigned vaddr = start; vaddr <= end; vaddr+=1024*4){
		int vpg = (vaddr - PAGE_OFFSET) >> 12;
		int ppg = vaddr >> 12;
		map_pg(dir, vpg, ppg, PG_USS, PG_RWW);	//equal map
		#if 1
		if(vaddr == end){		//only once
			static int done = false;
			if(done) break;
			done = true;

			end = 0xfec00000;
			vaddr = 0xfeb80000;
		}
		#endif
	}
	for(unsigned vaddr = 0xfdbf0000; vaddr <= 0xfdc00000; vaddr+=1024*4){
		int vpg = (vaddr - PAGE_OFFSET) >> 12;
		int ppg = vaddr >> 12;
		map_pg(dir, vpg, ppg, PG_USS, PG_RWW);	//equal map
	}
	oprintf(" temp mmio map done -----\n");
}
/**
 * |---kernel(1M)---|---kernel pgdir+kernel pgtbl(1M)---|---heap(14M)---|
 */
void mm_init(void){
	init_memory();
	init_zone();
	temp_mmio_map();
	extern bool mm_available;
	mm_available = true;
}

void mm_init2(void){
	mm_cache =  kmem_cache_create("mm_cache", sizeof(struct mm), 0,
										SLAB_HWCACHE_ALIGN, 0, 0);
	vm_area_cache = kmem_cache_create("vma_cache", sizeof(struct vm_area), 0,
										SLAB_HWCACHE_ALIGN, 0, 0);
}





