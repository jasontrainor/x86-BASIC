10 rem a demonstration for basic v2
20 print chr$(147);chr$(5);"colours, strings and numbers"
30 poke 53280,0:poke 53281,0
40 for i=0 to 15:poke 1024+120+i,160:poke 55296+120+i,i:next
50 a$="hello":b$=mid$(a$,2,3)+right$(a$,1)
60 print:print a$;" ";b$;len(b$);asc(a$)
70 dim s(10):for i=0 to 10:s(i)=i*i:next:print s(10)
80 def fn f(x)=x*x+1:print fn f(3)
90 t=ti:x%=-12345:print x%,int(-2.5),abs(-3)
100 read n$,v:print n$;v:data "score",100
110 if v>50 and v<200 then print "between":goto 130
120 print "outside"
130 gosub 200:print "back"
140 on 2 goto 150,160
150 print "no":end
160 print chr$(18);"reversed";chr$(146);" ";1/3;2^10
170 get k$:print "key ";len(k$)
180 print fre(0)
190 end
200 print "sub":return
