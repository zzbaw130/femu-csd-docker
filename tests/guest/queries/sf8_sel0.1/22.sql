-- SF8 target selectivity: 0.1
-- customer (inner positive balance and prefix): 303763/1200000 (selectivity=0.25313583333333333333)
-- customer (outer phone prefix): 334074/1200000 (selectivity=0.27839500000000000000)
\timing on

select
	cntrycode,
	count(*) as numcust,
	sum(c_acctbal) as totacctbal
from
	(
		select
			substring(c_phone from 1 for 2) as cntrycode,
			c_acctbal
		from
			customer
		where
			substring(c_phone from 1 for 2) in
				('10', '13', '24', '25', '26', '27', '30')
			and c_acctbal > (
				select
					avg(c_acctbal)
				from
					customer
				where
					c_acctbal > 0.00
					and substring(c_phone from 1 for 2) in
						('10', '13', '24', '25', '26', '27', '30')
			)
			and not exists (
				select
					*
				from
					orders
				where
					o_custkey = c_custkey
			)
	) as custsale
group by
	cntrycode
order by
	cntrycode;
