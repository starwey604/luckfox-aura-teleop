#!/usr/bin/env python3
"""Compile the actual SDK snapshot function with instrumented host OSAL shims."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get('AURA_SDK_ROOT', ROOT.parent / 'aura-sdk'))

SHIM = r'''
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int rk_u32;
typedef int rk_s32;
typedef unsigned long rk_ul;
typedef char rk_u8;
typedef void *KmppEnvNode;
typedef void *osal_spinlock;
typedef struct list_head { struct list_head *next, *prev; } osal_list_head;
#define OSAL_INIT_LIST_HEAD(h) do { (h)->next=(h); (h)->prev=(h); } while (0)
#define entry(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define osal_list_for_each_entry_safe(p,n,h,t,m) \
 for (p=entry((h)->next,t,m),n=entry(p->m.next,t,m); &p->m!=(h); p=n,n=entry(n->m.next,t,m))
#define min_t(t,a,b) ({ t x=(a),y=(b); x<y?x:y; })
#define max_t(t,a,b) ({ t x=(a),y=(b); x>y?x:y; })
static int irq, held, allocation_fail;
static void (*allocation_hook)(void);
static size_t bytes_allocated;
static char output[1024*1024];
static size_t output_len;
static void osal_spin_lock_irqsave(osal_spinlock *l, rk_ul *f) {
 (void)l; assert(!held); *f=(rk_ul)irq; irq=1; held=1;
}
static void osal_spin_unlock_irqrestore(osal_spinlock *l, rk_ul *f) {
 (void)l; assert(held); held=0; irq=(int)*f;
}
static void *kmpp_calloc(size_t n) {
 assert(!irq && !held); bytes_allocated=n;
 if (allocation_hook) allocation_hook();
 return allocation_fail?NULL:calloc(1,n);
}
static void kmpp_free(void *p) { assert(!irq && !held); free(p); }
static void kmpp_env_log(KmppEnvNode e, const char *fmt, ...) {
 va_list a; int n; (void)e; assert(!irq && !held);
 va_start(a,fmt); n=vsnprintf(output+output_len,sizeof(output)-output_len,fmt,a); va_end(a);
 assert(n>=0 && (size_t)n<sizeof(output)-output_len); output_len+=(size_t)n;
}
static void osal_strlcpy(char *d,const char *s,int n) {
 int i=0; while(i<n-1 && s[i]) { d[i]=s[i]; i++; } d[i]=0;
}
static void list_add(osal_list_head *n,osal_list_head *h) {
 n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;
}
'''

FIXTURE = r'''
static KmppMemPoolSrv server;
static KmppMemPoolImpl *make_pool(int id) {
 KmppMemPoolImpl *p=calloc(1,sizeof(*p)+64);assert(p);
 strcpy((char *)p->name,"snapshot-test");p->pool_id=id;p->size=512;
 OSAL_INIT_LIST_HEAD(&p->used);OSAL_INIT_LIST_HEAD(&p->unused);
 list_add(&p->list_srv,&server.list_pool);server.pool_cnt++;return p;
}
static void add_node(KmppMemPoolImpl *p,int used,int id) {
 KmppMemPoolNode *n=calloc(1,sizeof(*n));assert(n);n->id=id;n->size=64;n->ptr=n;
 list_add(&n->list,used?&p->used:&p->unused);
 if(used)p->used_count++;else p->unused_count++;
}
static void grow(void) { make_pool(2); }
static void reset(void) {
 memset(&server,0,sizeof(server));OSAL_INIT_LIST_HEAD(&server.list_pool);srv_mempool=&server;
 output[0]=0;output_len=0;bytes_allocated=0;allocation_fail=0;allocation_hook=NULL;
 assert(!irq && !held);
}
static void release_fixture(void) {
 osal_list_head *h=server.list_pool.next;
 while(h!=&server.list_pool) {
  KmppMemPoolImpl *p=entry(h,KmppMemPoolImpl,list_srv);h=h->next;
  osal_list_head *heads[2]={&p->used,&p->unused};
  for(int i=0;i<2;i++) {
   osal_list_head *n=heads[i]->next;
   while(n!=heads[i]) { osal_list_head *next=n->next;free(entry(n,KmppMemPoolNode,list));n=next; }
  }
  free(p);
 }
 assert(!irq && !held);
}
int main(void) {
 reset();srv_mempool=NULL;kmpp_mem_pool_show(NULL,NULL);assert(!output_len);
 reset();kmpp_mem_pool_show(NULL,NULL);assert(strstr(output,"no mem pools"));
 reset();KmppMemPoolImpl *p=make_pool(1);add_node(p,1,1);add_node(p,0,2);
 kmpp_mem_pool_show(NULL,NULL);assert(strstr(output,"snapshot-test") && strstr(output,"unused") && strstr(output,"used"));
 assert(!strstr(output,"truncated"));release_fixture();
 reset();make_pool(1);allocation_hook=grow;kmpp_mem_pool_show(NULL,NULL);
 assert(strstr(output,"truncated"));release_fixture();
 reset();p=make_pool(1);for(int i=0;i<5000;i++)add_node(p,0,i);
 kmpp_mem_pool_show(NULL,NULL);assert(strstr(output,"truncated"));
 assert(bytes_allocated==POOL_SNAPSHOT_MAX_ROWS*sizeof(KmppMemPoolSnapshot));release_fixture();
 reset();make_pool(1);allocation_fail=1;kmpp_mem_pool_show(NULL,NULL);
 assert(strstr(output,"allocation failed"));release_fixture();
 puts("PASS: null/empty/normal/concurrent growth/cap/OOM, no allocation or formatting under IRQ lock");
}
'''


class PoolSnapshotTest(unittest.TestCase):
    def test_production_snapshot_bounds_and_lock_context(self):
        source = (SDK / 'sysdrv/drv_ko/kmpp/kmpp/sys/kmpp_mem_pool.c').read_text()
        structs = source[source.index('typedef struct KmppMemPoolSrv_t'):source.index('static KmppMemPoolSrv *srv_mempool')]
        snapshot = source[source.index('/* Private debug snapshot:'):source.index('\n\nstatic void put_mem_pool')]
        prefix = '\nstatic KmppMemPoolSrv *srv_mempool;\n#define get_pool_srv_f() srv_mempool\nstatic const char space_12[13]="            ";\n'
        with tempfile.TemporaryDirectory() as td:
            path = Path(td); (path/'test.c').write_text(SHIM + structs + prefix + snapshot + FIXTURE)
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer', str(path/'test.c'), '-o', str(path/'test')], check=True)
            result = subprocess.run([str(path/'test')], text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS:', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
