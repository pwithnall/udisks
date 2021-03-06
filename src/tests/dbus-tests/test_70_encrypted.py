import dbus
import os

import udiskstestcase


class UdisksEncryptedTest(udiskstestcase.UdisksTestCase):
    '''This is an encrypted device test suite'''

    def _create_luks(self, device, passphrase):
        device.Format('xfs', {'encrypt.passphrase': passphrase},
                      dbus_interface=self.iface_prefix + '.Block')

    def _remove_luks(self, device, close=True):
        if close:
            try:
                device.Lock(self.no_options, dbus_interface=self.iface_prefix + '.Encrypted')
            except dbus.exceptions.DBusException as e:
                # ignore when luks is actually already locked
                if not str(e).endswith('is not unlocked'):
                    raise e

        d = dbus.Dictionary(signature='sv')
        d['erase'] = True
        device.Format('empty', d, dbus_interface=self.iface_prefix + '.Block')

    def _unmount(self, disk_path):
        self.run_command('umount %s' % disk_path)

    def test_create(self):
        disk_name = os.path.basename(self.vdevs[0])
        disk = self.get_object('/block_devices/' + disk_name)

        self._create_luks(disk, 'test')
        self.addCleanup(self._remove_luks, disk)
        self.udev_settle()

        # check dbus properties
        dbus_usage = self.get_property(disk, '.Block', 'IdUsage')
        dbus_usage.assertEqual('crypto')

        dbus_type = self.get_property(disk, '.Block', 'IdType')
        dbus_type.assertEqual('crypto_LUKS')

        device = self.get_property(disk, '.Block', 'Device')
        device.assertEqual(self.str_to_ay(self.vdevs[0]))  # device is an array of byte

        # check system values
        _ret, sys_type = self.run_command('lsblk -d -no FSTYPE %s' % self.vdevs[0])
        self.assertEqual(sys_type, 'crypto_LUKS')

        _ret, sys_uuid = self.run_command('lsblk -d -no UUID %s' % self.vdevs[0])
        dbus_uuid = self.get_property(disk, '.Block', 'IdUUID')
        dbus_uuid.assertEqual(sys_uuid)

        # get the luks device
        _ret, dm_name = self.run_command('ls /sys/block/%s/holders/' % disk_name)
        obj_name = 'dm_2d' + dm_name[-1]  # '-' is encoded as '_2d' in object paths
        luks = self.get_object('/block_devices/' + obj_name)

        self.assertIsNotNone(luks)

        # check dbus properties
        dbus_usage = self.get_property(luks, '.Block', 'IdUsage')
        dbus_usage.assertEqual('filesystem')

        dbus_type = self.get_property(luks, '.Block', 'IdType')
        dbus_type.assertEqual('xfs')

        device = self.get_property(luks, '.Block', 'Device')
        device.assertEqual(self.str_to_ay('/dev/' + dm_name))  # device is an array of byte

        crypto_dev = self.get_property(luks, '.Block', 'CryptoBackingDevice')
        crypto_dev.assertEqual(disk.object_path)

        # check system values
        _ret, sys_type = self.run_command('lsblk -d -no FSTYPE /dev/%s' % dm_name)
        self.assertEqual(sys_type, 'xfs')

        _ret, sys_uuid = self.run_command('lsblk -d -no UUID /dev/%s' % dm_name)
        bus_uuid = self.get_property(luks, '.Block', 'IdUUID')
        bus_uuid.assertEqual(sys_uuid)

    def test_close_open(self):
        disk_name = os.path.basename(self.vdevs[0])
        disk = self.get_object('/block_devices/' + disk_name)

        self._create_luks(disk, 'test')
        self.addCleanup(self._remove_luks, disk)
        self.udev_settle()

        # get the uuid of the luks device
        _ret, dm_name = self.run_command('ls /sys/block/%s/holders/' % disk_name)
        obj_name = 'dm_2d' + dm_name[-1]  # '-' is encoded as '_2d' in object paths
        luks = self.get_object('/block_devices/' + obj_name)
        self.assertIsNotNone(luks)

        _ret, luks_uuid = self.run_command('lsblk -d -no UUID /dev/%s' % dm_name)
        self.assertTrue(os.path.exists('/dev/disk/by-uuid/%s' % luks_uuid))

        disk.Lock(self.no_options, dbus_interface=self.iface_prefix + '.Encrypted')
        self.assertFalse(os.path.exists('/dev/disk/by-uuid/%s' % luks_uuid))

        # check that luks device disappears after lock
        udisks = self.get_object('')
        objects = udisks.GetManagedObjects(dbus_interface='org.freedesktop.DBus.ObjectManager')
        self.assertNotIn(str(luks.object_path), objects.keys())

        # wrong password
        msg = 'org.freedesktop.UDisks2.Error.Failed: Error unlocking %s *' % self.vdevs[0]
        with self.assertRaisesRegex(dbus.exceptions.DBusException, msg):
            disk.Unlock('shbdkjaf', self.no_options,
                        dbus_interface=self.iface_prefix + '.Encrypted')

        # right password
        luks = disk.Unlock('test', self.no_options,
                           dbus_interface=self.iface_prefix + '.Encrypted')
        self.assertIsNotNone(luks)
        self.assertTrue(os.path.exists('/dev/disk/by-uuid/%s' % luks_uuid))

    def test_mount(self):
        disk_name = os.path.basename(self.vdevs[0])
        disk = self.get_object('/block_devices/' + disk_name)

        self._create_luks(disk, 'test')
        self.addCleanup(self._remove_luks, disk)
        self.udev_settle()

        # get the luks object and mount it
        _ret, dm_name = self.run_command('ls /sys/block/%s/holders/' % disk_name)
        obj_name = 'dm_2d' + dm_name[-1]  # '-' is encoded as '_2d' in object paths
        luks = self.get_object('/block_devices/' + obj_name)
        self.assertIsNotNone(luks)

        mnt_path = luks.Mount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        self.assertIsNotNone(mnt_path)
        self.addCleanup(self._unmount, mnt_path)

        # should not be possible to close mounted luks
        msg = 'org.freedesktop.UDisks2.Error.Failed: Error locking'
        with self.assertRaisesRegex(dbus.exceptions.DBusException, msg):
            disk.Lock(self.no_options, dbus_interface=self.iface_prefix + '.Encrypted')

        # now unmount it and try to close it again
        luks.Unmount(self.no_options, dbus_interface=self.iface_prefix + '.Filesystem')
        disk.Lock(self.no_options, dbus_interface=self.iface_prefix + '.Encrypted')

    def test_password_change(self):
        disk_name = os.path.basename(self.vdevs[0])
        disk = self.get_object('/block_devices/' + disk_name)

        self._create_luks(disk, 'test')
        self.addCleanup(self._remove_luks, disk)
        self.udev_settle()

        disk.ChangePassphrase('test', 'password', self.no_options,
                              dbus_interface=self.iface_prefix + '.Encrypted')

        disk.Lock(self.no_options, dbus_interface=self.iface_prefix + '.Encrypted')

        # old password, should fail
        msg = 'org.freedesktop.UDisks2.Error.Failed: Error unlocking %s *' % self.vdevs[0]
        with self.assertRaisesRegex(dbus.exceptions.DBusException, msg):
            disk.Unlock('test', self.no_options,
                        dbus_interface=self.iface_prefix + '.Encrypted')

        # new password
        luks = disk.Unlock('password', self.no_options,
                           dbus_interface=self.iface_prefix + '.Encrypted')
        self.assertIsNotNone(luks)
