# Purpose
The purpose of the vdo-setup-dmtest package is to set up the repositories and environment necessary
for developing or running VDO tests through the dmtest-python utility.

# Description
The vdo-setup-dmtest package will install and configure dmtest-python, along with its blk-archive
and bufio-test dependencies, as they may be used for future VDO testing and/or test development.

In the case that installing and configuring blk-archive and bufio-test is not desired, their
key/value pairs and any packages they depend on can safely be commented out or removed from the
vdo-setup-dmtest YAML configuration file.

The vdo-setup-dmtest package assumes that the VDO device stack needed for testing has already been
created. Upon successful completion, it will display a list of any environment variables and their
values that need to be exported. This list can be returned as output with the specification of the
'--returnEnv' option at runtime.

# Installation

- Ensure python3 >= 3.7 and pip are installed.
- Install the vdo-setup-dmtest python dependencies:

```bash
 python3 -m pip install -r requirements.txt
```

- Edit config.yaml to meet your needs prior to running vdo-setup-dmtest. Alternately, you can
  specify the full path to the configuration file you would like to use with the '--config'
  option at runtime.

```bash
 python3 -m vdo-setup-dmtest --config <full path/name>
```

# Running

To run vdo-setup-dmtest, issue one of the following commands with the desired options:

```bash
 ./vdo-setup-dmtest.py <options>
```

```bash
 python3 -m vdo-setup-dmtest <options>
```

# Help

- Help text for vdo-setup-dmtest can be viewed by issuing one of the following commands:

```bash
 ./vdo-setup-dmtest.py --help
```

```bash
 python3 vdo-setup-dmtest.py --help
```

- Additional module information can be viewed through an interactive python session:

```bash
 python3
 help('vdo-setup-dmtest')
```

