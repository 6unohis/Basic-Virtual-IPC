# Virtual IPC Queue Module - Basic Version

Ubuntu 커널 모듈 기반 가상 IPC 메시지 큐 구현

## 개요

Linux 커널 모듈로 구현한 System V IPC와 유사한 메시지 큐 시스템입니다.
/proc 파일 시스템을 통해 사용자 공간과 통신하며, 프로세스 간 메시지 전달 기능을 제공합니다.
Makefile의 경우, 생성형 AI를 활용해 필요한 기능을 추가했습니다.

## 주요 기능

- 메시지 큐 생성/삭제
- 타입 기반 메시지 전송/수신
- /proc 인터페이스를 통한 제어
- 실시간 성능 모니터링
- 뮤텍스 기반 동기화

## 개발 환경

- OS: Ubuntu 20.04
- Kernel: Linux 5.15+
- Language: C (Kernel Module)

## 빌드 및 설치

### 필수 요구사항

```bash
sudo apt-get update
sudo apt-get install build-essential linux-headers-$(uname -r)
```

### 빌드

```bash
make
```

### 모듈 로드

```bash
sudo insmod vipc_queue_basic.ko
```

## 사용 방법

### 큐 생성

```bash
echo "create" | sudo tee /proc/vipc_queue_basic
```

### 메시지 전송

```bash
echo "send 0 1 HelloWorld" | sudo tee /proc/vipc_queue_basic
echo "send 0 2 TestMessage" | sudo tee /proc/vipc_queue_basic
```

### 상태 확인

```bash
cat /proc/vipc_queue_basic
```

### 커널 로그 확인

```bash
sudo dmesg | grep VIPC_BASIC | tail -20
```

### 모듈 언로드

```bash
sudo rmmod vipc_queue_basic
```

## 주요 구조체

### 메시지 구조체
```c
struct vipc_message {
    struct list_head list;
    long mtype;              // 메시지 타입
    size_t msize;            // 메시지 크기
    ktime_t timestamp;       // 생성 시각
    char mtext[];            // 메시지 내용
};
```

### 큐 구조체
```c
struct vipc_queue {
    int queue_id;
    struct list_head messages;
    struct mutex lock;
    int msg_count;
    size_t total_size;
};
```

## 제약사항

- 단일 큐 구조로 인한 락 경합 발생
- 8개 이상 프로세스 환경에서 성능 저하
- 중복 락 가능성으로 데드락 위험

## 문제 해결

### 모듈 로드 실패
```bash
ls /lib/modules/$(uname -r)/build
sudo apt-get install linux-headers-$(uname -r)
```

### "File exists" 에러
```bash
sudo rmmod vipc_queue_basic
sudo insmod vipc_queue_basic.ko
```

## 라이선스

GPL (GNU General Public License)