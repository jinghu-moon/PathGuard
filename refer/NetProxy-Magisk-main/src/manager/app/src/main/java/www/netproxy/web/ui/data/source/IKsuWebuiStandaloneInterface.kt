package www.netproxy.web.ui.data.source

import android.content.pm.PackageInfo
import android.os.Binder
import android.os.IBinder
import android.os.IInterface
import android.os.Parcel
import android.os.Parcelable
import android.os.RemoteException
import rikka.parcelablelist.ParcelableListSlice

/**
 * AIDL 生成的接口（手动 Kotlin 实现）
 * 
 * 用于跨进程获取已安装应用包列表
 * 由于 Windows AIDL 编译工具的 Unicode 路径问题，此接口手动实现
 */
interface IKsuWebuiStandaloneInterface : IInterface {
    
    /**
     * 获取已安装的应用包列表
     * @param flags PackageManager 查询标志
     * @return 应用包信息列表
     */
    @Throws(RemoteException::class)
    fun getPackages(flags: Int): ParcelableListSlice<PackageInfo>?
    
    /**
     * 默认实现
     */
    class Default : IKsuWebuiStandaloneInterface {
        override fun getPackages(flags: Int): ParcelableListSlice<PackageInfo>? = null
        override fun asBinder(): IBinder? = null
    }
    
    /**
     * 本地 IPC 实现存根类
     */
    abstract class Stub : Binder(), IKsuWebuiStandaloneInterface {
        
        init {
            attachInterface(this, DESCRIPTOR)
        }
        
        override fun asBinder(): IBinder = this
        
        @Throws(RemoteException::class)
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            val descriptor = DESCRIPTOR
            return when (code) {
                INTERFACE_TRANSACTION -> {
                    reply?.writeString(descriptor)
                    true
                }
                TRANSACTION_getPackages -> {
                    data.enforceInterface(descriptor)
                    val arg0 = data.readInt()
                    val result = this.getPackages(arg0)
                    reply?.writeNoException()
                    if (result != null && reply != null) {
                        reply.writeInt(1)
                        result.writeToParcel(reply, Parcelable.PARCELABLE_WRITE_RETURN_VALUE)
                    } else {
                        reply?.writeInt(0)
                    }
                    true
                }
                else -> super.onTransact(code, data, reply, flags)
            }
        }
        
        /**
         * 代理类
         */
        private class Proxy(private val remote: IBinder) : IKsuWebuiStandaloneInterface {
            
            override fun asBinder(): IBinder = remote
            
            fun getInterfaceDescriptor(): String = DESCRIPTOR
            
            @Suppress("UNCHECKED_CAST")
            @Throws(RemoteException::class)
            override fun getPackages(flags: Int): ParcelableListSlice<PackageInfo>? {
                val data = Parcel.obtain()
                val reply = Parcel.obtain()
                return try {
                    data.writeInterfaceToken(DESCRIPTOR)
                    data.writeInt(flags)
                    remote.transact(TRANSACTION_getPackages, data, reply, 0)
                    reply.readException()
                    if (reply.readInt() != 0) {
                        ParcelableListSlice.CREATOR.createFromParcel(reply) as ParcelableListSlice<PackageInfo>?
                    } else {
                        null
                    }
                } finally {
                    reply.recycle()
                    data.recycle()
                }
            }
        }
        
        companion object {
            private const val DESCRIPTOR = "www.netproxy.web.ui.data.source.IKsuWebuiStandaloneInterface"
            private const val TRANSACTION_getPackages = FIRST_CALL_TRANSACTION + 0
            
            /**
             * 将 IBinder 转换为接口，必要时生成代理
             */
            fun asInterface(obj: IBinder?): IKsuWebuiStandaloneInterface? {
                if (obj == null) return null
                val iin = obj.queryLocalInterface(DESCRIPTOR)
                return if (iin != null && iin is IKsuWebuiStandaloneInterface) {
                    iin
                } else {
                    Proxy(obj)
                }
            }
        }
    }
}
