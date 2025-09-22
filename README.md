# Nu System

Nu is a new datacenter system that enables developers to build fungible applications that can use datacenter resources wherever they are—even if they come from different machines—without the need of reserving them, thereby significantly improving resource efficiency. Currently, Nu supports C++ applications.

## Paper
* [Nu: Achieving Microsecond-Scale Resource Fungibility with Logical Processes](https://www.usenix.org/system/files/nsdi23-ruan.pdf)<br>
Zhenyuan Ruan, Seo Jin Park, Marcos Aguilera, Adam Belay, Malte Schwarzkopf<br>
20th USENIX Symposium on Networked Systems Design and Implementation ([NSDI '23](https://www.usenix.org/conference/nsdi23))<br>

## DDB-Moded Nu Runtime

This repo contains the DDB-moded Nu runtime, which is a modified version of the original Nu runtime that supports DDB for distributed interactive debugging. To check out the original readme for this repository, please see [README_orig.md](README_orig.md).

### Debug-aware Caladan Runtime

This repo also lightly modified the Caladan runtime, which is used by Nu, to allow the scheduler be aware of the debugger. With this modification, the scheduler will not try to preempt the uthread on the process if the process is paused by a debugger.

To run the debugger-aware mode:
``` bash
# in "caladan" root dir
sudo ./iokerneld ias dbg
```

The `dbg` flag specifies the debugger-aware mode. However, to specify the `dbg` flag, you need to first specify the schedule policy name, e.g., "ias".
