# 项目结构

```text
├─ kernel/
│  ├─ sysfile.c      # 新增/实现 sys_mmap、sys_munmap；vma_writeback_and_unmap；find_free_range / range_overlap / find_vma_covering；vma_cleanup
│  ├─ vm.c           # 新增 vmfault 的 VMA 分支：懒加载文件页、权限检查、mappages
│  ├─ proc.h         # 新增 struct vma、MAXVMA；在 struct proc 中加入 vmas[MAXVMA]
│  ├─ proc.c         # 初始化 vmas；fork 复制 VMA 并 filedup；kexit 调用 vma_cleanup
│  ├─ syscall.c      # 注册系统调用：mmap / munmap
│  └─ syscall.h      # 定义系统调用号：SYS_mmap / SYS_munmap
│
├─ user/
│  ├─ user.h         # 声明用户态接口：mmap / munmap
│  └─ usys.pl        # 生成用户态系统调用桩：mmap / munmap
│
└─ Makefile          # 构建规则
```
