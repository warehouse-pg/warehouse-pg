## For RHEL/Rocky8:

  - For EL versions (> 8.0):
    - Install git
      ```bash
      sudo yum install git
      ```
  - Install dependencies using README.Rhel-Rocky.bash script.
       ```bash
       ./README.Rhel-Rocky.bash
       ```


## Common Platform Tasks:

Make sure that you add `/usr/local/lib` to `/etc/ld.so.conf`,
then run command `ldconfig`.
1. Create gpadmin and setup ssh keys

   manually create ssh keys so you can do ssh localhost without a password, e.g., 
   
   ```
   ssh-keygen
   cat ~/.ssh/id_rsa.pub >> ~/.ssh/authorized_keys
   chmod 600 ~/.ssh/authorized_keys
   ```

1. Verify that you can ssh to your machine name without a password.

   ```bash
   ssh <hostname of your machine>  # e.g., ssh briarwood (You can use `hostname` to get the hostname of your machine.)
   ```

1. Set up your system configuration by following the installation guide on [docs.warehousepg.org](https://docs.warehousepg.org)


