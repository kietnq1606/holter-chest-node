# Hướng dẫn chia thread trong Zephyr

Thiết kế ứng dụng gồm ba thread: acquisition, storage và BLE + FSM.
Có thể dùng thread `main` cho BLE + FSM và chỉ tạo thêm hai thread để tiết kiệm stack RAM.
Zephyr vẫn có các thread hệ thống và Bluetooth nội bộ.

## 1. Mỗi thread cần những gì?

- **Entry:** hàm thực hiện công việc của thread.
- **Stack:** bộ nhớ riêng cho biến cục bộ và các lời gọi hàm.
- **Control block:** `struct k_thread`, được kernel dùng để quản lý thread.
- **Priority:** mức ưu tiên khi thread sẵn sàng chạy.
- **Cơ chế chờ:** queue, semaphore hoặc sự kiện để thread ngủ khi chưa có việc.

Chữ ký entry của Zephyr:

```c
void acquisition_thread_entry(void *p1, void *p2, void *p3);
```

Ba tham số có thể là `NULL` nếu không cần truyền dữ liệu lúc tạo thread.

## 2. Chia công việc và chọn ưu tiên

| Thread | Công việc | Priority minh họa |
|---|---|---|
| Acquisition | Chờ nhịp lấy mẫu, đọc sensor, gom mẫu thành block | 4 |
| Storage | Chờ yêu cầu, đọc/ghi Flash, cập nhật metadata | 5 |
| Main: BLE + FSM | Xử lý sự kiện BLE, START/STOP và gửi dữ liệu | 6 |

Trong Zephyr, số priority **nhỏ hơn có ưu tiên cao hơn**.
Priority không âm là preemptive: thread ưu tiên cao hơn có thể giành CPU khi sẵn sàng.
Priority âm là cooperative; nên dùng preemptive cho các thread ứng dụng này.

Các số `4`, `5`, `6` chỉ là ví dụ. Cần kiểm tra chúng nằm trong phạm vi
`CONFIG_NUM_PREEMPT_PRIORITIES` và phù hợp với priority của thread hệ thống/BLE.
Priority cao không tự bảo đảm nhịp ADC chính xác; đường lấy mẫu vẫn cần nguồn thời gian
phù hợp và không bị chặn bởi Flash hoặc BLE.

## 3. Cấp phát stack tĩnh

```c
#define ACQ_STACK_SIZE     1024
#define STORAGE_STACK_SIZE 1536

K_THREAD_STACK_DEFINE(acq_stack, ACQ_STACK_SIZE);
K_THREAD_STACK_DEFINE(storage_stack, STORAGE_STACK_SIZE);

static struct k_thread acq_thread;
static struct k_thread storage_thread;
```

Đây là kích thước minh họa, chưa được xác nhận đủ cho ứng dụng.
Mỗi thread có stack riêng; không dùng chung một stack cho hai thread đang tồn tại.
Dùng macro của Zephyr thay vì tự khai báo mảng byte để bảo đảm alignment và yêu cầu kiến trúc.

Khi tạo thread, truyền `K_THREAD_STACK_SIZEOF(acq_stack)` thay vì tự tính kích thước.
Cấp phát tĩnh không cần heap và tồn tại trong suốt thời gian chạy.

Thread `main` đã có stack, được cấu hình bằng `CONFIG_MAIN_STACK_SIZE`.
Nếu dùng main cho BLE + FSM, phải tính stack cho cả phần khởi tạo và xử lý BLE/FSM.

RAM còn bao gồm control block, queue, buffer pool và các thread hệ thống.
Không thể suy ra tổng RAM chỉ bằng cộng hai giá trị stack trong ví dụ.

Để chọn stack thực tế:

1. Tránh đặt buffer lớn trong biến cục bộ của thread.
2. Xét chuỗi lời gọi sâu nhất, đường xử lý lỗi và các API driver/BLE được gọi.
3. Khi thực hiện đo trên thiết bị, dùng khả năng theo dõi stack của Zephyr và chạy các đường xử lý nặng nhất.
4. Giữ khoảng dự phòng; không giảm stack chỉ dựa trên một lần chạy ngắn.

## 4. Tạo và khởi động thread trong main

Ví dụ dưới đây dùng main làm thread BLE + FSM. Đặt các khai báo stack/control block
ở file tạo thread để dễ quản lý; không khai báo thêm bản sao ở các file module.

```c
#include <zephyr/kernel.h>

#include "acquisition.h"
#include "storage_thread.h"
#include "ble_thread.h"

/* Example sizes; verify actual stack usage. */
#define ACQ_STACK_SIZE     1024
#define STORAGE_STACK_SIZE 1536

#define ACQ_PRIORITY      4
#define STORAGE_PRIORITY  5
#define BLE_APP_PRIORITY  6

K_THREAD_STACK_DEFINE(acq_stack, ACQ_STACK_SIZE);
K_THREAD_STACK_DEFINE(storage_stack, STORAGE_STACK_SIZE);

static struct k_thread acq_thread;
static struct k_thread storage_thread;

int main(void)
{
    int err;

    k_thread_priority_set(k_current_get(), BLE_APP_PRIORITY);

    err = ble_thread_init();
    if (err != 0) {
        return err;
    }

    err = storage_thread_init();
    if (err != 0) {
        return err;
    }

    err = acquisition_init();
    if (err != 0) {
        return err;
    }

    k_thread_create(&storage_thread,
                    storage_stack,
                    K_THREAD_STACK_SIZEOF(storage_stack),
                    storage_thread_entry,
                    NULL, NULL, NULL,
                    STORAGE_PRIORITY, 0, K_FOREVER);

    k_thread_create(&acq_thread,
                    acq_stack,
                    K_THREAD_STACK_SIZEOF(acq_stack),
                    acquisition_thread_entry,
                    NULL, NULL, NULL,
                    ACQ_PRIORITY, 0, K_FOREVER);

    k_thread_start(&storage_thread);
    k_thread_start(&acq_thread);

    /* Reuse the main thread for BLE and the application FSM. */
    ble_thread_entry(NULL, NULL, NULL);
    return 0;
}
```

Trong `k_thread_create()`:

- `&acq_thread`: control block của thread.
- `acq_stack`: vùng stack riêng.
- `K_THREAD_STACK_SIZEOF(...)`: kích thước stack phù hợp với macro khai báo.
- `acquisition_thread_entry`: hàm kernel sẽ gọi.
- `NULL, NULL, NULL`: tham số entry.
- `ACQ_PRIORITY`: priority của thread.
- `0`: không bật option bổ sung.
- `K_FOREVER`: tạo thread nhưng chưa cho chạy; dùng `k_thread_start()` để khởi động.

Có thể dùng `K_NO_WAIT` để thread được phép chạy ngay sau khi tạo.
Thread ưu tiên cao hơn có thể chạy trước khi `k_thread_create()` trả về, nên phải
khởi tạo xong mọi đối tượng mà nó sử dụng trước lúc đó.

Khởi động storage trước **không đồng nghĩa storage đã recovery xong**.
FSM phải chờ sự kiện storage ready trước khi cho phép START ghi dữ liệu.
Acquisition khi mới chạy cần ở trạng thái chờ lệnh, chưa lấy mẫu.
Lệnh START đến sớm cần được từ chối hoặc giữ lại theo chính sách ứng dụng.

## 5. Gọi thread khác bằng cách gửi lệnh

Sau khi tạo thread, không gọi trực tiếp `acquisition_thread_entry()` từ main.
Lời gọi hàm thông thường chạy trên stack của thread đang gọi, không tạo thread mới.

Thay vào đó, dùng API gửi lệnh:

```c
int err = acquisition_start();
if (err != 0) {
    /* Handle a rejected command. */
}
```

API này sẽ đưa lệnh START vào queue. Acquisition nhận lệnh và tự thay đổi trạng thái.
Giá trị trả về `0` chỉ xác nhận lệnh được nhận vào queue, chưa xác nhận thao tác hoàn tất.
Kết quả thực hiện cần được báo bằng sự kiện về BLE + FSM.

Phân biệt ba thao tác:

| Thao tác | Ý nghĩa |
|---|---|
| `k_thread_create()` | Tạo thread và gắn stack/entry/priority |
| `k_thread_start()` | Khởi động thread được tạo với `K_FOREVER` |
| `acquisition_start()` | Gửi lệnh bắt đầu phiên lấy mẫu cho thread đã chạy |

Không tạo lại thread mỗi lần START, và không dùng suspend/abort để dừng phiên đo.

## 6. Chờ việc và trao đổi buffer

Entry thường có vòng lặp xử lý, nhưng phải chờ khi chưa có việc:

- Storage: `k_msgq_get(..., K_FOREVER)` để chờ yêu cầu đọc/ghi/flush.
- BLE + FSM: chờ queue sự kiện; callback BLE chỉ gửi sự kiện ngắn.
- Acquisition: chờ START hoặc nhịp lấy mẫu; xử lý STOP theo cơ chế đánh thức đã chọn.

Không chạy vòng `while (1)` trống. Không dùng `k_msleep(2)` sau mỗi lần đọc
để mặc định rằng nhịp lấy mẫu chính xác là 500 Hz, vì thời gian xử lý còn cộng vào chu kỳ.
ISR/timer callback chỉ phát tín hiệu ngắn; không ghi/erase Flash ở ISR.

Luồng dữ liệu dùng queue và pool có giới hạn:

```text
Acquisition -> WRITE request -> Storage -> completion -> buffer pool
BLE + FSM  -> READ request  -> Storage -> completion -> BLE send
```

Queue chứa descriptor nhỏ/con trỏ, tránh sao chép cả block mẫu.
Buffer phải sống đủ lâu; không gửi con trỏ đến biến cục bộ sắp hết thời gian sống.
Khi gửi thành công, bên gửi không tái sử dụng buffer trước khi bên nhận báo hoàn tất.
Khi gửi thất bại, bên gửi giữ quyền sở hữu buffer và xử lý lỗi.

Acquisition gửi block không chờ (`K_NO_WAIT`); queue hoặc pool đầy phải có chính sách
quá tải rõ ràng, không âm thầm ghi đè mẫu chưa lưu.
Chỉ storage thread gọi API storage service để tránh tranh chấp buffer và metadata nội bộ.

Khi STOP:

1. Acquisition ngừng lấy mẫu và chuyển block cuối sang storage.
2. Acquisition báo đã chuyển xong dữ liệu cuối.
3. FSM gửi `storage_thread_submit_flush()`.
4. Storage báo hoàn tất sau khi dữ liệu và metadata đã commit thành công.
5. FSM mới chuyển về IDLE và báo đã dừng.

## 7. Áp dụng vào template hiện tại

Các API trong `applications/*.c` hiện là stub trả `-ENOSYS`; entry chưa có vòng lặp.
Cần tự implement init, queue, buffer pool, xử lý lệnh và thông báo hoàn tất trước khi chạy.
Ví dụ main sẽ dừng ở kiểm tra lỗi init nếu các stub vẫn giữ nguyên.

Khi tích hợp build, thêm các file `.c` vào `target_sources(app PRIVATE ...)` và thêm
`applications` vào đường dẫn include nếu main nằm ở thư mục khác.
Chọn đúng một file có `main()`; không liên kết đồng thời test main và application main.
File hướng dẫn này không thay đổi CMake hay cấu hình firmware.

Tài liệu chính thức:

- [Zephyr Thread APIs](https://docs.zephyrproject.org/latest/doxygen/html/group__thread__apis.html)
- [Zephyr Scheduling](https://docs.zephyrproject.org/latest/kernel/services/scheduling/index.html)
- [Zephyr Message Queues](https://docs.zephyrproject.org/latest/kernel/services/data_passing/message_queues.html)
