SQLEXEC is a sqlite3 extension which allows you to define virtual tables
in terms of SQL. Why do you want to do this? Well, you can't use PRAGMA
in queries, views, etc. Suppose you want to define a view which includes
the results of a sqlite3 PRAGMA call, you can't. But, with this extension
you can. For example:

```
sqlite> .load sqlexec
sqlite> create virtual table pragma_database_list
   ...> using sqlexec(pragma database_list);
sqlite> .head on
sqlite> select * from pragma_database_list;
seq|name|file
0|main|
```

Now, this works for PRAGMA database_list since it takes no arguments.
This doesn't really work other PRAGMAs such as PRAGMA table_info since
there is no way to dynamically specify an argument. I was thinking about
how to do that – I think you'd use xBestIndex and xFilter – but I
haven't implemented it.
