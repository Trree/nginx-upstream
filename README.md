# nginx-upstream

原始代码来源与 深入理解NGINX 的第五章。由于在最新的NGXIN分钟编译后无法得到预期的结果，所以重新修改运行。

nginx 访问第三方服务，如果使用 proxy-pass 指令无法满足需求，需要自己重新实现一个upstream或者subrequest。

代码演示了如何实现一个upstream的功能。
