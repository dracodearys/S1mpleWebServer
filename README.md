# S1mpleWebServer
初学者的Web服务器，应用`线程池 + 非阻塞socket + epollET + 模拟Proactor`实现

最大并发能力尚不清楚，由于虚拟机相关问题，只测试了8000的并发连接数

## 使用
由于代码构建简单，所以直接写的makefile
```
make server
./server port
```
然后在浏览器端访问`ip:port`即可

## TO DO
实现日志系统 

以及其他提高并发性能的优化
