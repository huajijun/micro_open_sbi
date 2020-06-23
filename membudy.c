Mem * start ;
void InitSpace(FreeList avail)
{
	int k;
	Mem *r = NULL;

	for(k = 0; k <=M; k++)
	{
		avail[k].size = pow(2,k);
		avail[k].first = NULL;
	}

	r = pow(2,M)*sizeof(Mem);
	r.prev = r;
	r.next = r;
	r.tag = 0;
	r.kval = M;

	avail[M].Head = r;
	start = r;
}
Mem* AllocBuddy(FreeList avail, int n)
{
	int k,i;
	Mem *pa,*pre,*next,*pi;
	for (k = 0; k <=M && (avail[k].size < n +1 || !(avail[k].first)); k++);

	if (k > M)
		return -1;
	pa = avail[k].first;
	pre = pa->pre;
	next = pa->next;
	if (pre == next)
		avail[k].first =NULL;
	else{
		?????
	}

	for (i = 1; k - i >= 0 && avail[k-i].size >= n+1;i++)
	{
		pi = pa + pow(2,k-i);
		pi->prev = pi->next = pi;
		pi.tag = 0;
		pi.kval = k -i;
		avail[k-i].first = pi;
	}
	pa.tag = 1;
	pa.kval = k - (--i);

	return pa;
}

void FreeBuddy(List avail , Mem *p)
{
	int k;
	Mem * buddy = Buddy(p);
}
static Mem* Buddy(Mem *p)
{
	long s , m ,n;
	if (p== NULL)
		return NULL;
	s = p - start;
	if (s <0)
		return NULL;
	m = (long)pow(2,p->kval);
	n = (long)pow(2,p->kval+1);

	if (s % n == 0)
		return p + m;

	if (s % n == m)
		return p -m;

	return NULL;
}