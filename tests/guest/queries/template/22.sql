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
				(:'q22_phone_prefix_1', :'q22_phone_prefix_2', :'q22_phone_prefix_3', :'q22_phone_prefix_4', :'q22_phone_prefix_5', :'q22_phone_prefix_6', :'q22_phone_prefix_7')
			and c_acctbal > (
				select
					avg(c_acctbal)
				from
					customer
				where
					c_acctbal > 0.00
					and substring(c_phone from 1 for 2) in
						(:'q22_phone_prefix_1', :'q22_phone_prefix_2', :'q22_phone_prefix_3', :'q22_phone_prefix_4', :'q22_phone_prefix_5', :'q22_phone_prefix_6', :'q22_phone_prefix_7')
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
