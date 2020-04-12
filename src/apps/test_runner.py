#!/usr/bin/env python 

# simple runner for RTA tests
# script expects "rta-wallet-cli" and "rta-pos-cli" binaries located in current directory
# script currently accepts two params (CLI interface will be extended to cover  all possible params "rta-wallet-cli" and "rta-pos-cli" currently supporting)
#
#  1. wallet-path - path to the client wallet to spend money from (client)
#  2. pos-wallet  - POS (merchant) wallet address to receive money


import subprocess
import threading
import argparse
import time

RTA_WALLET_APP = 'rta-wallet-cli'
RTA_POS_APP    = 'rta-pos-cli'
SUPERNODE_ADDRESS = 'localhost:28690'
CRYPTONODE_ADDRESS = 'localhost:28881'


lock = threading.Lock()

def run_client_wallet(wallet_path, cryptonode_address, supernode_address):
	cmd = ['./' + RTA_WALLET_APP , '--wallet-path',  wallet_path, '--cryptonode-address', cryptonode_address, '--supernode-address', supernode_address]
	
	proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.PIPE)
	for line in iter(proc.stdout.readline,''):
		print("WALLET: " + line.rstrip())
		if "Press <Return> to continue.." in line:
			# wait for POS started and initiated the sale
			global lock
			lock.acquire() 
			proc.stdin.write('\n')

	proc.stdout.close()
	return_code = proc.wait()
	if return_code:
		print("Wallet finished with error: %d" % return_code)


def run_pos(wallet_address, supernode_address):
	cmd = ['./' + RTA_POS_APP, '--wallet-address', wallet_address, '--supernode-address', supernode_address]
	proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.PIPE)
	for line in iter(proc.stdout.readline,''):
		print("POS: " + line.rstrip())
		if "Sale initiated" in line:
			global lock
			lock.release()
			

	proc.stdout.close()
	return_code = proc.wait()
	if return_code:
		print("POS finished with error: %d" % return_code)


## wallet - start, wait for wallet synchronized and refreshed, wait for pos started, continue
## pos    - start, wait for qr-code created, let wallet continue

def main():
	# command-line args: 
	# --wallet-path - path to a wallet to spend money from (client)
	# --pos-wallet  - pos wallet address to receive money (merchant)
	parser = argparse.ArgumentParser(description='RTA test helper')
	parser.add_argument("--wallet-path", type = str, required = True)
	parser.add_argument("--pos-wallet", type = str, required = True)
	parser.add_argument("--supernode-address", type = str, required = False)
	parser.add_argument("--cryptonode-address", type = str, required = False)
	parser.add_argument("--period", type = int, required = False, default = 120)

	args = parser.parse_args()

	supernode_address = SUPERNODE_ADDRESS
	if args.supernode_address is not None:
		supernode_address = args.supernode_address

	cryptonode_address = CRYPTONODE_ADDRESS
	if args.cryptonode_address is not None:
		cryptonode_address = args.cryptonode_address


	# acquire lock
	while True:
		print("RTA test started")
		lock.acquire()
		wallet_thread = threading.Thread(target = run_client_wallet, args = (args.wallet_path, cryptonode_address, supernode_address))
		wallet_thread.start()
		print("Wallet thread started");
		pos_thread = threading.Thread(target = run_pos, args = (args.pos_wallet, supernode_address))
		pos_thread.start()
		print("POS thread started")
		wallet_thread.join()
		pos_thread.join()
		lock.release()
		print("RTA test finished, sleeping for %d seconds" % args.period)
		time.sleep(args.period)


if __name__ == "__main__":
    main()    	

