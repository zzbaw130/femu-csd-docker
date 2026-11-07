-- SF8 target selectivity: 0.5
-- customer (inner positive balance and prefix): 307214/1200000 (selectivity=0.25601166666666666667)
-- customer (outer phone prefix): 337711/1200000 (selectivity=0.28142583333333333333)
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
				('15', '16', '19', '20', '21', '32', '33')
			and c_acctbal > (
				select
					avg(c_acctbal)
				from
					customer
				where
					c_acctbal > 0.00
					and substring(c_phone from 1 for 2) in
						('15', '16', '19', '20', '21', '32', '33')
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
