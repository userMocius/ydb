select t.a as a, c from t1 join (select t1.a as a, t2.b as c
from t1 join t2 using (a)) as t on t.a = t1.a order by a;
