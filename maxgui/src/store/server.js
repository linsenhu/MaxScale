/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-07-16
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

export default {
    namespaced: true,
    state: {
        all_servers: [],
        current_server: {},
        server_connections_chart_data: {
            datasets: [],
        },
    },
    mutations: {
        /**
         * @param {Array} payload // List of server resources
         */
        SET_ALL_SERVERS(state, payload) {
            state.all_servers = payload
        },
        SET_CURRENT_SERVER(state, payload) {
            state.current_server = payload
        },
        SET_SERVER_CONNECTION_CHART_DATA(state, payload) {
            state.server_connections_chart_data = payload
        },
    },
    actions: {
        async fetchAllServers({ commit }) {
            try {
                let res = await this.vue.$axios.get(`/servers`)
                if (res.data.data) {
                    // reverse array, latest will be last
                    let sorted = res.data.data.reverse()
                    commit('SET_ALL_SERVERS', sorted)
                }
            } catch (e) {
                if (process.env.NODE_ENV !== 'test') {
                    const logger = this.vue.$logger('store-server-fetchAllServers')
                    logger.error(e)
                }
            }
        },

        async fetchServerById({ commit }, id) {
            try {
                let res = await this.vue.$axios.get(`/servers/${id}`)
                if (res.data.data) commit('SET_CURRENT_SERVER', res.data.data)
            } catch (e) {
                if (process.env.NODE_ENV !== 'test') {
                    const logger = this.vue.$logger('store-server-fetchServerById')
                    logger.error(e)
                }
            }
        },

        //-----------------------------------------------Server Create/Update/Delete----------------------------------

        /**
         * @param {Object} payload payload object
         * @param {String} payload.id Name of the server
         * @param {Object} payload.parameters Parameters for the server
         * @param {Object} payload.relationships The relationships of the server to other resources
         * @param {Object} payload.relationships.services services object
         * @param {Object} payload.relationships.monitors monitors object
         * @param {Function} payload.callback callback function after successfully updated
         */
        async createServer({ commit }, payload) {
            try {
                const body = {
                    data: {
                        id: payload.id,
                        type: 'servers',
                        attributes: {
                            parameters: payload.parameters,
                        },
                        relationships: payload.relationships,
                    },
                }
                let res = await this.vue.$axios.post(`/servers/`, body)
                let message = [`Server ${payload.id} is created`]
                // response ok
                if (res.status === 204) {
                    commit(
                        'SET_SNACK_BAR_MESSAGE',
                        {
                            text: message,
                            type: 'success',
                        },
                        { root: true }
                    )
                    if (this.vue.$help.isFunction(payload.callback)) await payload.callback()
                }
            } catch (e) {
                if (process.env.NODE_ENV !== 'test') {
                    const logger = this.vue.$logger('store-server-createServer')
                    logger.error(e)
                }
            }
        },

        //-----------------------------------------------Server parameter update---------------------------------
        /**
         * @param {Object} payload payload object
         * @param {String} payload.id Name of the server
         * @param {Object} payload.parameters Parameters for the server
         * @param {Function} payload.callback callback function after successfully updated
         */
        async updateServerParameters({ commit }, payload) {
            try {
                const body = {
                    data: {
                        id: payload.id,
                        type: 'servers',
                        attributes: { parameters: payload.parameters },
                    },
                }
                let res = await this.vue.$axios.patch(`/servers/${payload.id}`, body)
                // response ok
                if (res.status === 204) {
                    commit(
                        'SET_SNACK_BAR_MESSAGE',
                        {
                            text: [`Parameters of ${payload.id} is updated`],
                            type: 'success',
                        },
                        { root: true }
                    )
                    if (this.vue.$help.isFunction(payload.callback)) await payload.callback()
                }
            } catch (e) {
                if (process.env.NODE_ENV !== 'test') {
                    const logger = this.vue.$logger('store-server-updateServerParameters')
                    logger.error(e)
                }
            }
        },

        //-----------------------------------------------Server relationship update---------------------------------
        /**
         * @param {Object} payload payload object
         * @param {String} payload.id Name of the server
         * @param {Array} payload.services services array
         * @param {Array} payload.monitors monitors array
         * @param {Object} payload.type Type of relationships
         * @param {Function} payload.callback callback function after successfully updated
         */
        async updateServerRelationship({ commit }, payload) {
            try {
                let res
                let message

                res = await this.vue.$axios.patch(
                    `/servers/${payload.id}/relationships/${payload.type}`,
                    {
                        data: payload.type === 'services' ? payload.services : payload.monitors,
                    }
                )

                message = [
                    `${this.vue.$help.capitalizeFirstLetter(payload.type)} relationships of ${
                        payload.id
                    } is updated`,
                ]

                // response ok
                if (res.status === 204) {
                    commit(
                        'SET_SNACK_BAR_MESSAGE',
                        {
                            text: message,
                            type: 'success',
                        },
                        { root: true }
                    )
                    if (this.vue.$help.isFunction(payload.callback)) await payload.callback()
                }
            } catch (e) {
                if (process.env.NODE_ENV !== 'test') {
                    const logger = this.vue.$logger('store-server-updateServerRelationship')
                    logger.error(e)
                }
            }
        },

        async destroyServer({ commit }, id) {
            try {
                let res = await this.vue.$axios.delete(`/servers/${id}?force=yes`)
                // response ok
                if (res.status === 204) {
                    commit(
                        'SET_SNACK_BAR_MESSAGE',
                        {
                            text: [`Server ${id} is deleted`],
                            type: 'success',
                        },
                        { root: true }
                    )
                }
            } catch (e) {
                if (process.env.NODE_ENV !== 'test') {
                    const logger = this.vue.$logger('store-server-destroyServer')
                    logger.error(e)
                }
            }
        },

        /**
         * @param {Object} param - An object.
         * @param {String} param.id id of the server
         * @param {String} param.state state of the server maintenance or drain)
         * @param {String} param.mode mode set or clear
         * @param {Function} param.callback callback function after successfully updated
         * @param {Boolean} param.forceClosing force all connections to the server to be closed immediately. Only works
         * for maintenance mode
         */
        async setOrClearServerState({ commit }, { id, state, mode, callback, forceClosing }) {
            try {
                let res, message

                switch (mode) {
                    case 'set':
                        {
                            let url = `/servers/${id}/set?state=${state}`
                            if (state === 'maintenance' && forceClosing)
                                url = url.concat('&force=yes')
                            res = await this.vue.$axios.put(url)
                            message = [`Server ${id} is set to ${state}`]
                        }
                        break
                    case 'clear':
                        res = await this.vue.$axios.put(`/servers/${id}/clear?state=${state}`)
                        message = [`State ${state} of server ${id} is cleared`]
                        break
                }
                // response ok
                if (res.status === 204) {
                    commit(
                        'SET_SNACK_BAR_MESSAGE',
                        { text: message, type: 'success' },
                        { root: true }
                    )
                    if (this.vue.$help.isFunction(callback)) await callback()
                }
            } catch (e) {
                if (process.env.NODE_ENV !== 'test') {
                    const logger = this.vue.$logger('store-server-setOrClearServerState')
                    logger.error(e)
                }
            }
        },

        /**
         *  Generate data schema for total connections of each server
         */
        genDataSetSchema({ commit, state }) {
            const { all_servers } = state
            const { dynamicColors, strReplaceAt } = this.vue.$help

            if (all_servers.length) {
                let arr = []
                all_servers.forEach((server, i) => {
                    const {
                        id,
                        attributes: { statistics: { connections = null } = {} } = {},
                    } = server

                    const lineColor = dynamicColors(i)
                    const indexOfOpacity = lineColor.lastIndexOf(')') - 1
                    const backgroundColor = strReplaceAt(lineColor, indexOfOpacity, '0.1')
                    const obj = {
                        label: `Server ID - ${id}`,
                        id: `Server ID - ${id}`,
                        type: 'line',
                        // background of the line
                        backgroundColor: backgroundColor,
                        borderColor: lineColor,
                        borderWidth: 1,
                        lineTension: 0,
                        data: [{ x: Date.now(), y: connections }],
                    }
                    arr.push(obj)
                })

                let serversConnectionsChartDataSchema = {
                    datasets: arr,
                }
                commit('SET_SERVER_CONNECTION_CHART_DATA', serversConnectionsChartDataSchema)
            }
        },
    },
    getters: {
        // -------------- below getters are available only when fetchAllServers has been dispatched
        getAllServersMap: state => {
            let map = new Map()
            state.all_servers.forEach(ele => {
                map.set(ele.id, ele)
            })
            return map
        },

        getAllServersInfo: state => {
            let idArr = []
            let portNumArr = []
            return state.all_servers.reduce((accumulator, _, index, array) => {
                idArr.push(array[index].id)
                portNumArr.push(array[index].attributes.parameters.port)

                return (accumulator = { idArr: idArr, portNumArr: portNumArr })
            }, [])
        },
    },
}
