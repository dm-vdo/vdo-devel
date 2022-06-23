# vdorecover
VDO Recovery from a full disk condition, using snapshots above and below VDO
and temporary storage in /tmp. 

Especially useful on bare-metal where underlying disks cannot increase in size
the way they can in a VM. (Note VDO doesn't allow aggregating storage into one
backing store device)

Used successfully by Red Hat customers to recover VDO devices when no other
method would have worked - see
https://access.redhat.com/solutions/3520581#comment-2100841 
