# Project Contribution & Team Boundaries

The **HPC Temperature Alert System** was a collaborative university engineering project designed and built by a team of 7 students (Group 9). 

This document clarifies the project boundaries, distinguishing between team-wide deliverables and the specific, individual engineering contributions of **Ferdaws Qaem** (GitHub: [@Ferdaws-c](https://github.com/Ferdaws-c)).

---

## 🔍 Work Breakdown Structure

Below is a breakdown of the overall system components and the ownership of each module:

| System Layer | Description / Component | Ownership | Ferdaws' Specific Role |
| :--- | :--- | :--- | :--- |
| **IoT Hardware** | Physical circuit, wiring, pin assignments, breadboard assembly. | **Individual** | Selected and wired the DHT11, DHT22, 16x2 LCD I2C interface, and PWM-driven RGB LED to the ESP32. |
| **IoT Firmware** | C++ Arduino codebase, sensor reading drivers, non-blocking loops, status alerts, display output. | **Individual** | Wrote 100% of the ESP32 code (`src/`), including local threshold comparison logic and display logic. |
| **Network & Sync** | Wi-Fi client connection, WPA2 Enterprise setup, HTTP POST requests, dynamic threshold GET requests. | **Individual** | Implemented non-blocking connection recovery, database HTTP POSTs, and dynamic threshold fetching. |
| **Fault Tolerance** | Off-grid robustness, caching logs on power/network loss, exponential backoffs. | **Individual** | Engineered the C++ Ring Buffer cache and the exponential backoff retry algorithms to handle network downtime. |
| **Cloud Backend** | Supabase database tables, PostgreSQL trigger functions, and row-level security (RLS). | **Shared / Team** | Collaborated on defining database schemas and payload formats for the sensor readings. |
| **Frontend Web** | Next.js/React portal displaying live temperatures, history graphs, and setting threshold rules. | **Shared / Team** | Integrated the ESP32 HTTP layer with the endpoints defined by the frontend/backend developers. |
| **System Docs** | Software Requirements Specification (SRS) and the final IEEE Research Paper. | **Shared / Team** | Authored chapters detailing the physical hardware module, firmware flowcharts, and local safety logic. |

---

## 📜 Academic Integrity & Open Source Policy

*   **Firmware Autonomy:** All source files (`.ino` and `.h` files) located in the `src/` folder of this repository were written exclusively by Ferdaws Qaem. No source code from other group members is included in this repository.
*   **Documentation Credits:** The team-level documents provided in `docs/reference/` (`IEEE.docx` and `SRS_Group_9.docx`) represent the collective intellectual property of all 7 team members. They are included in this repository under "reference" solely to provide context for academic reviewers and technical recruiters assessing the firmware's environment.
*   **Permissions:** All group members agreed to showcase the final system and references in individual portfolios to demonstrate team collaboration.

---

## 💡 Key Technical Skills Demonstrated by Ferdaws

By reviewing the codebase in this repository, recruiters can assess my proficiency in:
1.  **Embedded C/C++ Development:** Utilizing pointer arithmetic, low-overhead structures, and static memory allocations.
2.  **Resource-Constrained Multitasking:** Implementing cooperative multitasking with time-slicing state machines (avoiding CPU blocking).
3.  **Network Engineering (IoT Sync):** REST API consumption on embedded hardware, JSON serialization/deserialization, and Enterprise WPA2 network authentication.
4.  **Resilient Embedded Architecture:** Design patterns for hardware failure, connection timeouts, and network retry state machines.
