#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/ktime.h>

#define MODULE_NAME "vipc_queue_basic"
#define PROC_NAME "vipc_queue_basic"
#define MAX_MSG_SIZE 4096
#define MAX_QUEUES 1  // 초기 버전: 단일 큐만 사용

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student Project - Basic Version");
MODULE_DESCRIPTION("Virtual IPC Queue Module - Basic (Before Optimization)");

/* 메시지 구조체 */
struct vipc_message {
    struct list_head list;
    long mtype;
    size_t msize;
    ktime_t timestamp;
    char mtext[];
};

/* 큐 구조체 */
struct vipc_queue {
    int queue_id;
    struct list_head messages;
    struct mutex lock;  // 단일 락으로 모든 작업 보호
    int msg_count;
    size_t total_size;
    int in_use;
    
    // 성능 측정용
    u64 total_operations;
    u64 total_lock_wait_time_ns;
};

static struct vipc_queue queues[MAX_QUEUES];
static struct proc_dir_entry *proc_entry;
static DEFINE_MUTEX(queues_lock);

/* 락 획득 시간 측정을 위한 헬퍼 */
static inline u64 measure_lock_time(struct mutex *lock)
{
    ktime_t start, end;
    start = ktime_get();
    mutex_lock(lock);
    end = ktime_get();
    return ktime_to_ns(ktime_sub(end, start));
}

/* 큐 생성 */
static int vipc_create_queue(void)
{
    int i;
    
    mutex_lock(&queues_lock);
    for (i = 0; i < MAX_QUEUES; i++) {
        if (!queues[i].in_use) {
            queues[i].queue_id = i;
            INIT_LIST_HEAD(&queues[i].messages);
            mutex_init(&queues[i].lock);
            queues[i].msg_count = 0;
            queues[i].total_size = 0;
            queues[i].in_use = 1;
            queues[i].total_operations = 0;
            queues[i].total_lock_wait_time_ns = 0;
            mutex_unlock(&queues_lock);
            printk(KERN_INFO "VIPC_BASIC: Queue %d created\n", i);
            return i;
        }
    }
    mutex_unlock(&queues_lock);
    return -1;
}

/* 메시지 전송 - 기본 버전 (중복 락 가능성 있음) */
static int vipc_send_message(int qid, long mtype, const char *mtext, size_t msize)
{
    struct vipc_message *msg;
    u64 lock_wait_ns;
    ktime_t start, end;
    
    if (qid < 0 || qid >= MAX_QUEUES || !queues[qid].in_use)
        return -EINVAL;
    
    if (msize > MAX_MSG_SIZE)
        return -EINVAL;
    
    msg = kmalloc(sizeof(struct vipc_message) + msize, GFP_KERNEL);
    if (!msg)
        return -ENOMEM;
    
    msg->mtype = mtype;
    msg->msize = msize;
    msg->timestamp = ktime_get();
    memcpy(msg->mtext, mtext, msize);
    
    start = ktime_get();
    
    // 문제점: 단일 락으로 모든 작업 직렬화
    lock_wait_ns = measure_lock_time(&queues[qid].lock);
    
    list_add_tail(&msg->list, &queues[qid].messages);
    queues[qid].msg_count++;
    queues[qid].total_size += msize;
    queues[qid].total_operations++;
    queues[qid].total_lock_wait_time_ns += lock_wait_ns;
    
    end = ktime_get();
    
    mutex_unlock(&queues[qid].lock);
    
    printk(KERN_INFO "VIPC_BASIC: Send Q%d type=%ld lock_wait=%lluns total_time=%lldns\n",
           qid, mtype, lock_wait_ns, ktime_to_ns(ktime_sub(end, start)));
    
    return 0;
}

/* 메시지 수신 - 기본 버전 */
static int vipc_receive_message(int qid, long mtype, char *mtext, 
                                size_t maxsize, size_t *actual_size)
{
    struct vipc_message *msg, *tmp;
    int found = 0;
    u64 lock_wait_ns;
    ktime_t start, end, msg_latency;
    
    if (qid < 0 || qid >= MAX_QUEUES || !queues[qid].in_use)
        return -EINVAL;
    
    start = ktime_get();
    lock_wait_ns = measure_lock_time(&queues[qid].lock);
    
    list_for_each_entry_safe(msg, tmp, &queues[qid].messages, list) {
        if (mtype == 0 || msg->mtype == mtype) {
            size_t copy_size = min(msg->msize, maxsize);
            memcpy(mtext, msg->mtext, copy_size);
            *actual_size = msg->msize;
            
            msg_latency = ktime_sub(ktime_get(), msg->timestamp);
            
            list_del(&msg->list);
            queues[qid].msg_count--;
            queues[qid].total_size -= msg->msize;
            queues[qid].total_operations++;
            queues[qid].total_lock_wait_time_ns += lock_wait_ns;
            
            kfree(msg);
            
            found = 1;
            
            end = ktime_get();
            printk(KERN_INFO "VIPC_BASIC: Recv Q%d type=%ld lock_wait=%lluns "
                   "msg_latency=%lldns total_time=%lldns\n",
                   qid, mtype, lock_wait_ns, 
                   ktime_to_ns(msg_latency),
                   ktime_to_ns(ktime_sub(end, start)));
            break;
        }
    }
    
    mutex_unlock(&queues[qid].lock);
    
    return found ? 0 : -ENOMSG;
}

/* 통계 조회 - 문제점: 이미 락을 잡고 있을 수 있음 (중복 락) */
static void vipc_get_stats(int qid)
{
    u64 avg_lock_wait_ns = 0;
    
    // 문제: 외부에서 이미 락을 잡고 이 함수를 호출하면 데드락
    mutex_lock(&queues[qid].lock);
    
    if (queues[qid].total_operations > 0) {
        avg_lock_wait_ns = queues[qid].total_lock_wait_time_ns / 
                          queues[qid].total_operations;
    }
    
    printk(KERN_INFO "VIPC_BASIC: Q%d stats - ops=%llu avg_lock_wait=%lluns\n",
           qid, queues[qid].total_operations, avg_lock_wait_ns);
    
    mutex_unlock(&queues[qid].lock);
}

/* /proc 파일 읽기 */
static int vipc_proc_show(struct seq_file *m, void *v)
{
    int i;
    
    seq_printf(m, "Virtual IPC Queue Status (BASIC VERSION)\n");
    seq_printf(m, "=========================================\n");
    seq_printf(m, "WARNING: This is the basic version with known performance issues\n");
    seq_printf(m, "- Single queue causes lock contention\n");
    seq_printf(m, "- No lock-free paths\n");
    seq_printf(m, "- Possible deadlock in nested calls\n\n");
    
    mutex_lock(&queues_lock);
    for (i = 0; i < MAX_QUEUES; i++) {
        if (queues[i].in_use) {
            u64 avg_lock_wait = 0;
            
            mutex_lock(&queues[i].lock);
            
            if (queues[i].total_operations > 0) {
                avg_lock_wait = queues[i].total_lock_wait_time_ns / 
                               queues[i].total_operations;
            }
            
            seq_printf(m, "Queue ID: %d\n", i);
            seq_printf(m, "  Messages: %d\n", queues[i].msg_count);
            seq_printf(m, "  Total Size: %zu bytes\n", queues[i].total_size);
            seq_printf(m, "  Total Operations: %llu\n", queues[i].total_operations);
            seq_printf(m, "  Avg Lock Wait: %llu ns (%llu µs)\n", 
                      avg_lock_wait, avg_lock_wait / 1000);
            seq_printf(m, "\n");
            
            mutex_unlock(&queues[i].lock);
        }
    }
    mutex_unlock(&queues_lock);
    
    return 0;
}

static int vipc_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, vipc_proc_show, NULL);
}

static ssize_t vipc_proc_write(struct file *file, const char __user *buffer,
                               size_t count, loff_t *pos)
{
    char cmd[256];
    char op[32];
    int qid;
    long mtype;
    char msg[MAX_MSG_SIZE];
    
    if (count >= sizeof(cmd))
        return -EINVAL;
    
    if (copy_from_user(cmd, buffer, count))
        return -EFAULT;
    
    cmd[count] = '\0';
    
    if (sscanf(cmd, "%s", op) != 1)
        return -EINVAL;
    
    if (strcmp(op, "create") == 0) {
        qid = vipc_create_queue();
        if (qid >= 0)
            printk(KERN_INFO "VIPC_BASIC: Created queue %d\n", qid);
    } else if (strcmp(op, "send") == 0) {
        if (sscanf(cmd, "send %d %ld %s", &qid, &mtype, msg) == 3) {
            vipc_send_message(qid, mtype, msg, strlen(msg) + 1);
        }
    } else if (strcmp(op, "stats") == 0) {
        if (sscanf(cmd, "stats %d", &qid) == 1) {
            vipc_get_stats(qid);
        }
    }
    
    return count;
}

static const struct proc_ops vipc_proc_ops = {
    .proc_open = vipc_proc_open,
    .proc_read = seq_read,
    .proc_write = vipc_proc_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static int __init vipc_init(void)
{
    int i;
    
    for (i = 0; i < MAX_QUEUES; i++) {
        queues[i].in_use = 0;
        queues[i].queue_id = -1;
    }
    
    proc_entry = proc_create(PROC_NAME, 0666, NULL, &vipc_proc_ops);
    if (!proc_entry) {
        printk(KERN_ERR "VIPC_BASIC: Failed to create /proc entry\n");
        return -ENOMEM;
    }
    
    printk(KERN_INFO "VIPC_BASIC: Module loaded (Basic version with performance issues)\n");
    
    return 0;
}

static void __exit vipc_exit(void)
{
    int i;
    struct vipc_message *msg, *tmp;
    
    for (i = 0; i < MAX_QUEUES; i++) {
        if (queues[i].in_use) {
            mutex_lock(&queues[i].lock);
            list_for_each_entry_safe(msg, tmp, &queues[i].messages, list) {
                list_del(&msg->list);
                kfree(msg);
            }
            mutex_unlock(&queues[i].lock);
        }
    }
    
    proc_remove(proc_entry);
    
    printk(KERN_INFO "VIPC_BASIC: Module unloaded\n");
}

module_init(vipc_init);
module_exit(vipc_exit);
